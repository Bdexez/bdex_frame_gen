#include "depth_tracker.h"

#include "device.h"
#include "log.h"

#include <iterator>

namespace bdex {

// (depth::choose and the format / layout helpers: depth_select.cpp, in
// bdex_core with the unit tests.)

namespace {
const float kNoClear = std::numeric_limits<float>::quiet_NaN();
} // namespace

DepthTracker::~DepthTracker() {
    for (auto& [h, img] : images_)
        if (img.view) dev_.vt.DestroyImageView(dev_.device, img.view, nullptr);
}

bool DepthTracker::isTracked(uint64_t image) const {
    if (!image) return false;
    for (const auto& s : fast_)
        if (s.load(std::memory_order_relaxed) == image) return true;
    if (fastFull_.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(mutex_);
        return images_.count(image) != 0;
    }
    return false;
}

void DepthTracker::addFast(uint64_t image) {
    for (auto& s : fast_) {
        uint64_t zero = 0;
        if (s.compare_exchange_strong(zero, image)) return;
    }
    fastFull_.store(true);
}

void DepthTracker::removeFast(uint64_t image) {
    for (auto& s : fast_) {
        uint64_t v = image;
        if (s.compare_exchange_strong(v, 0)) return;
    }
}

VkImageUsageFlags DepthTracker::usageFor(const VkImageCreateInfo& ci) const {
    VkImageUsageFlags usage = ci.usage;
    if (!depth::isDepthFormat(ci.format) || ci.imageType != VK_IMAGE_TYPE_2D || ci.samples != VK_SAMPLE_COUNT_1_BIT ||
        !(usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) || ci.extent.width < 64 || ci.extent.height < 64 ||
        ci.tiling != VK_IMAGE_TILING_OPTIMAL)
        return usage;
    // Not an image shared with another API or process: its usage is part of
    // what both sides agreed on.
    for (auto* p = static_cast<const VkBaseInStructure*>(ci.pNext); p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO) return usage;
    if (!(usage & VK_IMAGE_USAGE_SAMPLED_BIT) && dev_.formatSupports(ci.format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
        usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    return usage;
}

void DepthTracker::imageCreated(VkImage image, const VkImageCreateInfo& ci) {
    if (!depth::isDepthFormat(ci.format) || !(ci.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) return;
    Image img;
    img.format = ci.format;
    img.extent = {ci.extent.width, ci.extent.height};
    img.sampleable = (ci.usage & VK_IMAGE_USAGE_SAMPLED_BIT) && ci.samples == VK_SAMPLE_COUNT_1_BIT &&
                     ci.imageType == VK_IMAGE_TYPE_2D;
    img.layout = ci.initialLayout;
    const uint64_t h = handleBits(image);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        images_[h] = img;
    }
    addFast(h);
}

void DepthTracker::imageDestroying(VkImage image) {
    const uint64_t h = handleBits(image);
    if (!isTracked(h)) return;
    VkImageView view = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = images_.find(h);
        if (it == images_.end()) return;
        view = it->second.view;
        queue = it->second.readQueue;
        images_.erase(it);
        for (auto v = views_.begin(); v != views_.end();)
            v = v->second == h ? views_.erase(v) : std::next(v);
        if (picked_ == h) picked_ = 0;
    }
    removeFast(h);
    if (view) {
        // Our last read of it (an import on the game's queue) may still run:
        // the game waited for its own work, not ours.
        if (queue) {
            std::lock_guard<std::mutex> qlock(dev_.queueMutex(queue));
            dev_.vt.QueueWaitIdle(queue);
        }
        dev_.vt.DestroyImageView(dev_.device, view, nullptr);
    }
}

void DepthTracker::viewCreated(VkImageView view, const VkImageViewCreateInfo& ci) {
    const uint64_t h = handleBits(ci.image);
    if (!isTracked(h)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    views_[handleBits(view)] = h;
}

void DepthTracker::viewDestroyed(VkImageView view) {
    std::lock_guard<std::mutex> lock(mutex_);
    views_.erase(handleBits(view));
}

void DepthTracker::framebufferCreated(VkFramebuffer fb, const VkFramebufferCreateInfo& ci) {
    if (ci.flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) return;  // (views at vkCmdBeginRenderPass)
    std::vector<uint64_t> att(ci.attachmentCount, 0);
    std::lock_guard<std::mutex> lock(mutex_);
    bool any = false;
    for (uint32_t i = 0; i < ci.attachmentCount; ++i) {
        auto it = views_.find(handleBits(ci.pAttachments[i]));
        if (it != views_.end()) { att[i] = it->second; any = true; }
    }
    if (any) framebuffers_[handleBits(fb)] = std::move(att);
}

void DepthTracker::framebufferDestroyed(VkFramebuffer fb) {
    std::lock_guard<std::mutex> lock(mutex_);
    framebuffers_.erase(handleBits(fb));
}

void DepthTracker::renderPassCreated(VkRenderPass rp, const VkRenderPassCreateInfo& ci) {
    if (ci.subpassCount == 0 || !ci.pSubpasses[0].pDepthStencilAttachment) return;
    RenderPass r;
    // The depth attachment of any subpass (the first that has one).
    for (uint32_t s = 0; s < ci.subpassCount && r.depth < 0; ++s)
        if (const VkAttachmentReference* d = ci.pSubpasses[s].pDepthStencilAttachment; d && d->attachment != VK_ATTACHMENT_UNUSED)
            r.depth = int(d->attachment);
    if (r.depth < 0 || uint32_t(r.depth) >= ci.attachmentCount) return;
    r.loadOp = ci.pAttachments[r.depth].loadOp;
    r.finalLayout = ci.pAttachments[r.depth].finalLayout;
    std::lock_guard<std::mutex> lock(mutex_);
    renderPasses_[handleBits(rp)] = r;
}

void DepthTracker::renderPassCreated2(VkRenderPass rp, const VkRenderPassCreateInfo2& ci) {
    RenderPass r;
    for (uint32_t s = 0; s < ci.subpassCount && r.depth < 0; ++s)
        if (const VkAttachmentReference2* d = ci.pSubpasses[s].pDepthStencilAttachment; d && d->attachment != VK_ATTACHMENT_UNUSED)
            r.depth = int(d->attachment);
    if (r.depth < 0 || uint32_t(r.depth) >= ci.attachmentCount) return;
    r.loadOp = ci.pAttachments[r.depth].loadOp;
    r.finalLayout = ci.pAttachments[r.depth].finalLayout;
    std::lock_guard<std::mutex> lock(mutex_);
    renderPasses_[handleBits(rp)] = r;
}

void DepthTracker::renderPassDestroyed(VkRenderPass rp) {
    std::lock_guard<std::mutex> lock(mutex_);
    renderPasses_.erase(handleBits(rp));
}

void DepthTracker::queueRetrieved(VkQueue queue, uint32_t family) {
    std::lock_guard<std::mutex> lock(mutex_);
    queues_[queue] = family;
}

bool DepthTracker::queueFamily(VkQueue queue, uint32_t& family) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = queues_.find(queue);
    if (it == queues_.end()) return false;
    family = it->second;
    return true;
}

void DepthTracker::beginCommandBuffer(VkCommandBuffer cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cbs_.find(cb);
    if (it != cbs_.end()) {
        it->second.events.clear();
        it->second.lastDepth = 0;
    }
}

DepthTracker::Event* DepthTracker::push(VkCommandBuffer cb, const Event& e) {
    Cb& c = cbs_[cb];
    c.events.push_back(e);
    return &c.events.back();
}

void DepthTracker::passEvent(VkCommandBuffer cb, uint64_t image, VkImageLayout after, const VkRect2D& area, float clear) {
    // (mutex_ held)
    push(cb, Event{Event::Pass, image, after, area, clear});
    cbs_[cb].lastDepth = image;
}

void DepthTracker::beginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo& bi) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto rp = renderPasses_.find(handleBits(bi.renderPass));
    if (rp == renderPasses_.end()) return;
    const uint32_t idx = uint32_t(rp->second.depth);
    uint64_t image = 0;
    // An imageless framebuffer gets its views here.
    for (auto* p = static_cast<const VkBaseInStructure*>(bi.pNext); p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) {
            const auto* ab = reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(p);
            if (idx < ab->attachmentCount) {
                auto v = views_.find(handleBits(ab->pAttachments[idx]));
                if (v != views_.end()) image = v->second;
            }
        }
    if (!image) {
        auto fb = framebuffers_.find(handleBits(bi.framebuffer));
        if (fb == framebuffers_.end() || idx >= fb->second.size()) return;
        image = fb->second[idx];
    }
    if (!image) return;
    const float clear = rp->second.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && bi.pClearValues && idx < bi.clearValueCount
                            ? bi.pClearValues[idx].depthStencil.depth : kNoClear;
    passEvent(cb, image, rp->second.finalLayout, bi.renderArea, clear);
}

void DepthTracker::beginRendering(VkCommandBuffer cb, const VkRenderingInfo& ri) {
    const VkRenderingAttachmentInfo* d = ri.pDepthAttachment;
    if (!d || !d->imageView) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto v = views_.find(handleBits(d->imageView));
    if (v == views_.end()) return;
    const float clear = d->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? d->clearValue.depthStencil.depth : kNoClear;
    // (Dynamic rendering keeps the layout it renders in.)
    passEvent(cb, v->second, d->imageLayout, ri.renderArea, clear);
}

void DepthTracker::barriers(VkCommandBuffer cb, uint32_t count, const VkImageMemoryBarrier* b) {
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t h = handleBits(b[i].image);
        if (!isTracked(h) || !(b[i].subresourceRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)) continue;
        if (b[i].subresourceRange.baseMipLevel != 0 || b[i].subresourceRange.baseArrayLayer != 0) continue;
        std::lock_guard<std::mutex> lock(mutex_);
        push(cb, Event{Event::Layout, h, b[i].newLayout, {}, kNoClear});
    }
}

void DepthTracker::barriers2(VkCommandBuffer cb, const VkDependencyInfo& di) {
    for (uint32_t i = 0; i < di.imageMemoryBarrierCount; ++i) {
        const VkImageMemoryBarrier2& b = di.pImageMemoryBarriers[i];
        const uint64_t h = handleBits(b.image);
        if (!isTracked(h) || !(b.subresourceRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)) continue;
        if (b.subresourceRange.baseMipLevel != 0 || b.subresourceRange.baseArrayLayer != 0) continue;
        std::lock_guard<std::mutex> lock(mutex_);
        push(cb, Event{Event::Layout, h, b.newLayout, {}, kNoClear});
    }
}

void DepthTracker::clearImage(VkCommandBuffer cb, VkImage image, float value) {
    const uint64_t h = handleBits(image);
    if (!isTracked(h)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    push(cb, Event{Event::Clear, h, VK_IMAGE_LAYOUT_UNDEFINED, {}, value});
}

void DepthTracker::clearAttachments(VkCommandBuffer cb, uint32_t count, const VkClearAttachment* a) {
    for (uint32_t i = 0; i < count; ++i) {
        if (!(a[i].aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)) continue;
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cbs_.find(cb);
        if (it == cbs_.end() || !it->second.lastDepth) return;
        push(cb, Event{Event::Clear, it->second.lastDepth, VK_IMAGE_LAYOUT_UNDEFINED, {}, a[i].clearValue.depthStencil.depth});
        return;
    }
}

void DepthTracker::submitted(uint32_t count, const VkCommandBuffer* cbs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cbs_.empty()) return;
    for (uint32_t i = 0; i < count; ++i) {
        auto it = cbs_.find(cbs[i]);
        if (it == cbs_.end()) continue;
        for (const Event& e : it->second.events) {
            auto img = images_.find(e.image);
            if (img == images_.end()) continue;
            Image& m = img->second;
            switch (e.kind) {
            case Event::Pass: {
                if (e.layout != VK_IMAGE_LAYOUT_UNDEFINED) m.layout = e.layout;
                ++m.passes;
                if (uint64_t(e.area.extent.width) * e.area.extent.height > uint64_t(m.area.extent.width) * m.area.extent.height)
                    m.area = e.area;
                if (!std::isnan(e.clear)) m.clear = e.clear;
                break;
            }
            case Event::Layout: m.layout = e.layout; break;
            case Event::Clear: m.clear = e.clear; break;
            }
        }
    }
}

bool DepthTracker::pick(VkExtent2D target, Pick& out, std::string& why) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<depth::Usage> used;
    for (const auto& [h, img] : images_)
        if (img.passes) used.push_back({h, img.extent, img.area, img.passes, img.sampleable});
    const int i = depth::choose(used, target, picked_);
    for (auto& [h, img] : images_) {
        img.passes = 0;
        img.area = {};
    }
    if (i < 0) {
        why = used.empty() ? "no depth buffer drawn into this frame" : "no depth buffer of the picture's size";
        return false;
    }
    const uint64_t h = used[size_t(i)].image;
    Image& img = images_[h];
    VkImageLayout read;
    if (!depth::readLayout(img.layout, read)) {
        why = "the depth buffer's layout is not known";
        return false;
    }
    if (std::isnan(img.clear)) {
        why = "the depth buffer is never cleared (near/far order unknown)";
        return false;
    }
    if (!img.view) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = handleFromBits<VkImage>(h);
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = img.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (dev_.vt.CreateImageView(dev_.device, &vi, nullptr, &img.view) != VK_SUCCESS) {
            img.view = VK_NULL_HANDLE;
            why = "the depth buffer cannot be read";
            return false;
        }
    }
    if (h != picked_)
        BDEX_INFO("depth: %ux%u (format %d, drawn %ux%u at %d,%d), %u passes a frame, %s Z", img.extent.width,
                  img.extent.height, int(img.format), used[size_t(i)].area.extent.width, used[size_t(i)].area.extent.height,
                  used[size_t(i)].area.offset.x, used[size_t(i)].area.offset.y, used[size_t(i)].passes,
                  img.clear < 0.5f ? "reversed" : "standard");
    picked_ = h;
    out.image = handleFromBits<VkImage>(h);
    out.view = img.view;
    out.format = img.format;
    out.extent = img.extent;
    out.area = used[size_t(i)].area;
    out.layout = img.layout;
    out.reversed = img.clear < 0.5f;
    out.passes = used[size_t(i)].passes;
    why.clear();
    return true;
}

void DepthTracker::readOn(VkImage image, VkQueue queue) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = images_.find(handleBits(image));
    if (it != images_.end()) it->second.readQueue = queue;
}

} // namespace bdex
