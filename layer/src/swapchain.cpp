#include "swapchain.h"
#include "log.h"
#include "vk_util.h"

#include <algorithm>
#include <functional>

namespace bdex {

namespace {

constexpr uint32_t kAppSlots = 4;
constexpr uint32_t kWorkerSlots = 8;

bool presentModeSupported(DeviceData& dev, VkSurfaceKHR surface, VkPresentModeKHR mode) {
    uint32_t n = 0;
    if (dev.inst->vt.GetPhysicalDeviceSurfacePresentModesKHR(dev.physDev, surface, &n, nullptr) < 0) return false;
    std::vector<VkPresentModeKHR> modes(n);
    if (dev.inst->vt.GetPhysicalDeviceSurfacePresentModesKHR(dev.physDev, surface, &n, modes.data()) < 0) return false;
    return std::find(modes.begin(), modes.end(), mode) != modes.end();
}

} // namespace

VirtualSwapchain::VirtualSwapchain(DeviceData& dev, const VkSwapchainCreateInfoKHR& appInfo) : dev_(dev) {
    format_ = appInfo.imageFormat;
    extent_ = appInfo.imageExtent;
    try {
        createReal(appInfo);
        const bool wantGeneration = dev_.config.enabled && dev_.config.debug != Config::Debug::Passthrough && dev_.config.multiplier > 1;
        createVirtualImages(appInfo, wantGeneration);
        createSlots(slots_, kAppSlots, false);
        createSlots(workerSlots_, kWorkerSlots, true);
        std::vector<VkImage> sources;
        for (auto& vi : virtualImages_) sources.push_back(vi.img.image);
        if (wantGeneration) {
            try {
                fg_ = std::make_unique<FrameGen>(dev_, format_, extent_, static_cast<uint32_t>(dev_.config.multiplier - 1), sources);
                generating_ = true;
            } catch (const VkError& e) {
                BDEX_WARN("frame generation unavailable for this swapchain (%s); passing frames through", e.what());
            }
        }
        if (!generating_) fg_ = std::make_unique<FrameGen>(dev_, format_, extent_, 0, sources);
        worker_ = std::thread([this] { workerLoop(); });
    } catch (...) {
        destroyAll();
        throw;
    }
    statsStart_ = clock::now();
    dumping_ = generating_ && !dev_.config.dumpDir.empty();
    static uint32_t nextDumpId = 0;
    dumpId_ = nextDumpId++;
    BDEX_INFO("swapchain %ux%u format %d: %zu virtual images, %zu real images, present mode %s, generation %s",
              extent_.width, extent_.height, static_cast<int>(format_), virtualImages_.size(), realImages_.size(),
              presentModeName(presentMode_), generating_ ? "on" : "off");
}

VirtualSwapchain::~VirtualSwapchain() { destroyAll(); }

void VirtualSwapchain::createReal(const VkSwapchainCreateInfoKHR& appInfo) {
    VkSwapchainCreateInfoKHR ci = appInfo;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.queueFamilyIndexCount = 0;
    ci.pQueueFamilyIndices = nullptr;

    // The pNext chain is forwarded unchanged (format lists, scaling, present
    // modes...): it describes the surface-side behaviour which stays the same.
    if (dev_.config.presentMode >= 0 &&
        findChain<VkBaseInStructure>(appInfo.pNext, VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT)) {
        BDEX_WARN("application declares its present modes (VK_EXT_swapchain_maintenance1); ignoring present_mode override");
    } else if (dev_.config.presentMode >= 0) {
        auto mode = static_cast<VkPresentModeKHR>(dev_.config.presentMode);
        if (presentModeSupported(dev_, appInfo.surface, mode)) ci.presentMode = mode;
        else BDEX_WARN("present mode %s not supported by the surface, keeping %s", presentModeName(mode), presentModeName(appInfo.presentMode));
    } else if (dev_.config.presentMode == -2 &&
               (ci.presentMode == VK_PRESENT_MODE_FIFO_KHR || ci.presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR) &&
               presentModeSupported(dev_, appInfo.surface, VK_PRESENT_MODE_MAILBOX_KHR)) {
        // FIFO queues our extra frames in the presentation engine, which is
        // pure display latency (2-3 refreshes measured), and a FIFO present
        // can block for a refresh while we hold a queue the game may share.
        // Mailbox never blocks nor tears, and our pacing keeps the cadence.
        BDEX_INFO("presenting with mailbox instead of %s (present_mode=app keeps the game's mode)",
                  presentModeName(ci.presentMode));
        ci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    }
    presentMode_ = ci.presentMode;

    VkSurfaceCapabilitiesKHR caps{};
    if (dev_.inst->vt.GetPhysicalDeviceSurfaceCapabilitiesKHR(dev_.physDev, appInfo.surface, &caps) == VK_SUCCESS) {
        // We present `multiplier` frames per game frame: with too few images
        // the acquire of the real frame blocks behind the generated ones
        // still queued in the presentation engine, which delays it.
        const uint32_t mult = static_cast<uint32_t>(std::max(1, dev_.config.multiplier));
        uint32_t want = std::max(appInfo.minImageCount, 2u + mult);
        if (caps.maxImageCount) want = std::min(want, caps.maxImageCount);
        ci.minImageCount = std::max(want, caps.minImageCount);
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            throw VkError(VK_ERROR_FEATURE_NOT_PRESENT, "surface does not support transfer destination images");
    }

    VkBool32 supported = VK_FALSE;
    dev_.inst->vt.GetPhysicalDeviceSurfaceSupportKHR(dev_.physDev, dev_.queueFamily, appInfo.surface, &supported);
    if (!supported) throw VkError(VK_ERROR_FEATURE_NOT_PRESENT, "layer queue family cannot present to this surface");

    VkResult r = dev_.vt.CreateSwapchainKHR(dev_.device, &ci, nullptr, &real_);
    if (r < 0) throw VkError(r, "vkCreateSwapchainKHR (real)");

    uint32_t n = 0;
    VK_CHECK(dev_.vt.GetSwapchainImagesKHR(dev_.device, real_, &n, nullptr));
    std::vector<VkImage> imgs(n);
    VK_CHECK(dev_.vt.GetSwapchainImagesKHR(dev_.device, real_, &n, imgs.data()));
    realImages_.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        realImages_[i].image = imgs[i];
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(dev_.vt.CreateSemaphore(dev_.device, &si, nullptr, &realImages_[i].readySem));
    }
}

void VirtualSwapchain::createVirtualImages(const VkSwapchainCreateInfoKHR& appInfo, bool forGeneration) {
    // The presented frame stays held by the layer until the next present
    // (the worker copies it to the real swapchain, and the next synthesis
    // reads it as the previous frame), so the application gets one image
    // more than it asked for.
    uint32_t count = std::max(appInfo.minImageCount, 3u) + 1u;
    VkImageCreateFlags flags = 0;
    if (appInfo.flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    const void* next = findChain<VkImageFormatListCreateInfo>(appInfo.pNext, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO);
    VkImageFormatListCreateInfo list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    if (next) {
        list = *static_cast<const VkImageFormatListCreateInfo*>(next);
        list.pNext = nullptr;
        next = &list;
    }
    std::vector<uint32_t> families = dev_.appFamilies;
    if (appInfo.imageSharingMode == VK_SHARING_MODE_CONCURRENT)
        families.assign(appInfo.pQueueFamilyIndices, appInfo.pQueueFamilyIndices + appInfo.queueFamilyIndexCount);
    if (std::find(families.begin(), families.end(), dev_.queueFamily) == families.end()) families.push_back(dev_.queueFamily);
    std::sort(families.begin(), families.end());
    families.erase(std::unique(families.begin(), families.end()), families.end());

    // Surfaces may advertise usage bits that regular images of this format do
    // not support (e.g. STORAGE on sRGB); keep only what the format allows.
    VkFormatProperties fp{};
    dev_.inst->vt.GetPhysicalDeviceFormatProperties(dev_.physDev, format_, &fp);
    const VkFormatFeatureFlags feats = fp.optimalTilingFeatures;
    VkImageUsageFlags usage = appInfo.imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (forGeneration) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    auto dropUnless = [&](VkImageUsageFlags bit, VkFormatFeatureFlags feature) {
        if ((usage & bit) && !(feats & feature)) usage &= ~bit;
    };
    dropUnless(VK_IMAGE_USAGE_SAMPLED_BIT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    dropUnless(VK_IMAGE_USAGE_STORAGE_BIT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
    dropUnless(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
    dropUnless(VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
    dropUnless(VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
    if (usage & ~(appInfo.imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
        BDEX_WARN("dropped unsupported usage bits 0x%x from the swapchain images",
                  appInfo.imageUsage & ~usage);

    virtualImages_.resize(count);
    for (auto& vi : virtualImages_) {
        vi.img = dev_.createImage(format_, extent_, usage, false, flags, next, &families);
    }
}

void VirtualSwapchain::createSlots(std::vector<FrameSlot>& slots, uint32_t count, bool withSemaphore) {
    slots.resize(count);
    for (auto& s : slots) {
        VK_CHECK(dev_.allocateCommandBuffer(&s.cmd));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(dev_.vt.CreateFence(dev_.device, &fi, nullptr, &s.fence));
        if (withSemaphore) {
            VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VK_CHECK(dev_.vt.CreateSemaphore(dev_.device, &si, nullptr, &s.acquireSem));
        }
    }
}

void VirtualSwapchain::destroySlots(std::vector<FrameSlot>& slots) {
    for (auto& s : slots) {
        if (s.submitted) dev_.vt.WaitForFences(dev_.device, 1, &s.fence, VK_TRUE, UINT64_MAX);
        if (s.cmd) dev_.vt.FreeCommandBuffers(dev_.device, dev_.cmdPool, 1, &s.cmd);
        if (s.fence) dev_.vt.DestroyFence(dev_.device, s.fence, nullptr);
        if (s.acquireSem) dev_.vt.DestroySemaphore(dev_.device, s.acquireSem, nullptr);
    }
    slots.clear();
}

// Waits until every queued job has submitted its GPU work: from then on the
// internal images may be overwritten (queue order protects the GPU side).
// The worker may still be blocked in the final vkQueuePresentKHR, which is
// exactly what lets the application render its next frame in the meantime.
void VirtualSwapchain::waitWorkerSubmitted(std::unique_lock<std::mutex>& lock) {
    idleCv_.wait(lock, [this] { return unsubmitted_ == 0; });
}

void VirtualSwapchain::waitWorkerIdle(std::unique_lock<std::mutex>& lock) {
    idleCv_.wait(lock, [this] { return jobs_.empty() && !running_; });
}

void VirtualSwapchain::waitIdle() {
    std::lock_guard<std::mutex> lock(mutex_);
    {
        std::unique_lock<std::mutex> wlock(workerMutex_);
        waitWorkerIdle(wlock);
    }
    for (auto& s : slots_)
        if (s.submitted) dev_.vt.WaitForFences(dev_.device, 1, &s.fence, VK_TRUE, UINT64_MAX);
    for (auto& s : workerSlots_)
        if (s.submitted) dev_.vt.WaitForFences(dev_.device, 1, &s.fence, VK_TRUE, UINT64_MAX);
    std::lock_guard<std::mutex> qlock(dev_.queueMutex(dev_.queue));
    dev_.vt.QueueWaitIdle(dev_.queue);
}

void VirtualSwapchain::retire() {
    std::lock_guard<std::mutex> lock(mutex_);
    retired_ = true;
    std::unique_lock<std::mutex> wlock(workerMutex_);
    waitWorkerIdle(wlock);
}

void VirtualSwapchain::destroyAll() {
    auto& vt = dev_.vt;
    VkDevice d = dev_.device;
    if (worker_.joinable()) {
        {
            std::lock_guard<std::mutex> wlock(workerMutex_);
            stop_ = true;
        }
        workerCv_.notify_all();
        worker_.join();
    }
    destroySlots(slots_);
    destroySlots(workerSlots_);
    if (dev_.queue) {
        // Acquire semaphores signalled by a real acquire but never waited on
        // cannot be reclaimed without a queue wait.
        std::lock_guard<std::mutex> qlock(dev_.queueMutex(dev_.queue));
        vt.QueueWaitIdle(dev_.queue);
    }
    fg_.reset();
    for (auto& vi : virtualImages_) dev_.destroyImage(vi.img);
    virtualImages_.clear();
    for (auto& ri : realImages_) {
        if (ri.readySem) vt.DestroySemaphore(d, ri.readySem, nullptr);
    }
    realImages_.clear();
    if (real_) vt.DestroySwapchainKHR(d, real_, nullptr);
    real_ = VK_NULL_HANDLE;
}

VkResult VirtualSwapchain::getImages(uint32_t* count, VkImage* images) {
    const uint32_t n = static_cast<uint32_t>(virtualImages_.size());
    if (!images) {
        *count = n;
        return VK_SUCCESS;
    }
    const uint32_t m = std::min(*count, n);
    for (uint32_t i = 0; i < m; ++i) images[i] = virtualImages_[i].img.image;
    *count = m;
    return m < n ? VK_INCOMPLETE : VK_SUCCESS;
}

void VirtualSwapchain::waitVirtualRelease(VirtualImage& vi) {
    if (!vi.pendingRelease) return;
    FrameSlot& s = slots_[vi.releaseSlot];
    if (s.generation == vi.releaseGeneration && s.submitted) {
        dev_.vt.WaitForFences(dev_.device, 1, &s.fence, VK_TRUE, UINT64_MAX);
    }
    vi.pendingRelease = false;
}

VkResult VirtualSwapchain::acquire(uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* index) {
    std::lock_guard<std::mutex> lock(mutex_);
    (void)timeout;

    VkResult pending;
    {
        std::lock_guard<std::mutex> wlock(workerMutex_);
        pending = pendingResult_;
        if (pending != VK_ERROR_OUT_OF_DATE_KHR) pendingResult_ = VK_SUCCESS;
    }
    if (pending == VK_ERROR_OUT_OF_DATE_KHR || retired_) return VK_ERROR_OUT_OF_DATE_KHR;

    const uint32_t n = static_cast<uint32_t>(virtualImages_.size());
    int chosen = -1;
    for (uint32_t k = 0; k < n; ++k) {
        uint32_t i = (acquireCursor_ + k) % n;
        if (!virtualImages_[i].acquired && !virtualImages_[i].held) { chosen = static_cast<int>(i); break; }
    }
    if (chosen < 0) {
        BDEX_WARN("application acquired every swapchain image");
        return VK_NOT_READY;
    }
    VirtualImage& vi = virtualImages_[chosen];
    waitVirtualRelease(vi);
    vi.acquired = true;
    acquireCursor_ = static_cast<uint32_t>(chosen) + 1;
    *index = static_cast<uint32_t>(chosen);

    if (semaphore || fence) {
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.signalSemaphoreCount = semaphore ? 1 : 0;
        si.pSignalSemaphores = &semaphore;
        VkResult r = dev_.submit(si, fence);
        if (r < 0) {
            BDEX_ERR("failed to signal acquire semaphore: %s", vkResultName(r));
            return r;
        }
    }
    return pending;
}

VkResult VirtualSwapchain::release(uint32_t count, const uint32_t* indices) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t i = 0; i < count; ++i)
        if (indices[i] < virtualImages_.size()) virtualImages_[indices[i]].acquired = false;
    return VK_SUCCESS;
}

VirtualSwapchain::FrameSlot& VirtualSwapchain::nextSlot(std::vector<FrameSlot>& slots, uint32_t& cursor) {
    FrameSlot& s = slots[cursor];
    cursor = (cursor + 1) % slots.size();
    if (s.submitted) {
        dev_.vt.WaitForFences(dev_.device, 1, &s.fence, VK_TRUE, UINT64_MAX);
        dev_.vt.ResetFences(dev_.device, 1, &s.fence);
        s.submitted = false;
        fg_->profileCollect(static_cast<uint32_t>((&slots == &slots_ ? 0 : slots_.size()) + (&s - slots.data())));
    }
    ++s.generation;
    dev_.vt.ResetCommandBuffer(s.cmd, 0);
    return s;
}

VkResult VirtualSwapchain::consumeSemaphores(const VkSemaphore* sems, uint32_t count) {
    if (!count) return VK_SUCCESS;
    std::vector<VkPipelineStageFlags> stages(count, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = count;
    si.pWaitSemaphores = sems;
    si.pWaitDstStageMask = stages.data();
    return dev_.submit(si, VK_NULL_HANDLE);
}

void VirtualSwapchain::dumpFrame(FrameSlot& slot, bool generated, float t) {
    if (dumpCount_ >= dev_.config.dumpFrames) { dumping_ = false; return; }
    dev_.vt.WaitForFences(dev_.device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
    char name[96];
    snprintf(name, sizeof(name), "/sc%u_%ux%u_%04d_%s_t%.2f.ppm", dumpId_, extent_.width, extent_.height, dumpCount_,
             generated ? "gen" : "real", t);
    const std::string path = dev_.config.dumpDir + name;
    if (fg_->writeDump(path, generated ? 0 : 1)) BDEX_DBG("dumped %s", path.c_str());
    else BDEX_WARN("could not write %s", path.c_str());
    ++dumpCount_;
}

// ---------------------------------------------------------------------------
// Application side
// ---------------------------------------------------------------------------

VkResult VirtualSwapchain::present(uint32_t imageIndex, const VkPresentInfoKHR& info, bool consumeWaitSemaphores) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto t0 = clock::now();
    const VkSemaphore* appWaits = consumeWaitSemaphores ? info.pWaitSemaphores : nullptr;
    const uint32_t appWaitCount = consumeWaitSemaphores ? info.waitSemaphoreCount : 0;

    if (imageIndex >= virtualImages_.size() || retired_) {
        consumeSemaphores(appWaits, appWaitCount);
        return VK_ERROR_OUT_OF_DATE_KHR;
    }
    VirtualImage& vi = virtualImages_[imageIndex];
    vi.acquired = false;

    // The previous job must have been fully submitted by the worker before
    // we overwrite the images it copies from; this is also where FIFO
    // back-pressure reaches the application.
    VkResult pending;
    {
        std::unique_lock<std::mutex> wlock(workerMutex_);
        // Low-latency: do not let the game run ahead of the presentation
        // engine (with FIFO, queued frames are pure display latency).
        if (dev_.config.lowLatency) waitWorkerIdle(wlock);
        else waitWorkerSubmitted(wlock);
        pending = pendingResult_;
        if (pending != VK_ERROR_OUT_OF_DATE_KHR) pendingResult_ = VK_SUCCESS;
    }
    if (pending == VK_ERROR_OUT_OF_DATE_KHR) {
        consumeSemaphores(appWaits, appWaitCount);
        return pending;
    }

    // Frame interval estimate (EMA) for pacing.
    if (lastAppPresent_.time_since_epoch().count() != 0) {
        double dt = std::chrono::duration<double, std::milli>(t0 - lastAppPresent_).count();
        if (dt > 0.5 && dt < 500.0) frameIntervalMs_ = frameIntervalMs_ * 0.8 + dt * 0.2;
    }
    lastAppPresent_ = t0;

    const int mult = generating_ ? std::max(1, dev_.config.multiplier) : 1;
    const bool interpolate = generating_ && prevIndex_ >= 0 && mult > 1;
    const int prev = prevIndex_;

    // Synthesis: analyse the application's frame (in place, no copy) and
    // generate the intermediate frames into the internal output images.
    FrameSlot& slot = nextSlot(slots_, slotCursor_);
    const uint32_t slotIdx = static_cast<uint32_t>(&slot - slots_.data());
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dev_.vt.BeginCommandBuffer(slot.cmd, &bi);
    fg_->profileBegin(slot.cmd, slotIdx);
    if (generating_) {
        fg_->recordAcquireSources(slot.cmd, prev, static_cast<int>(imageIndex));
        fg_->recordAnalysis(slot.cmd, parity_, imageIndex);
        fg_->profileMark(slot.cmd, slotIdx, FrameGen::StageAnalysis);
        if (interpolate) {
            fg_->recordFlow(slot.cmd, parity_);
            fg_->profileMark(slot.cmd, slotIdx, FrameGen::StageFlow);
            // Interpolation: frames between prev and cur (t in (0,1)).
            // Extrapolation: frames after cur (t in (1,2)), shown after it.
            const float base = dev_.config.extrapolate ? 1.f : 0.f;
            for (int i = 1; i < mult; ++i)
                fg_->recordInterpolate(slot.cmd, static_cast<uint32_t>(prev), imageIndex, base + float(i) / mult, static_cast<uint32_t>(i - 1));
            fg_->profileMark(slot.cmd, slotIdx, FrameGen::StageInterp);
        }
        if (dumping_) {
            if (interpolate) fg_->recordDump(slot.cmd, 0, imageIndex);
            fg_->recordDump(slot.cmd, 1, imageIndex);
        }
        fg_->recordReleaseSources(slot.cmd, prev, static_cast<int>(imageIndex));
    }
    // Pass-through: an empty submission still consumes the application's
    // semaphores and orders the worker's copy after its rendering.
    dev_.vt.EndCommandBuffer(slot.cmd);

    std::vector<VkPipelineStageFlags> stages(appWaitCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = appWaitCount;
    si.pWaitSemaphores = appWaits;
    si.pWaitDstStageMask = stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &slot.cmd;
    VkResult r = dev_.submit(si, slot.fence);
    if (r < 0) {
        BDEX_ERR("synthesis submission failed: %s", vkResultName(r));
        consumeSemaphores(appWaits, appWaitCount);
        return r;
    }
    slot.submitted = true;
    // The previous frame was read for the last time by this submission; the
    // current one stays held until the next present reads it as `prev`.
    if (prev >= 0) {
        VirtualImage& pv = virtualImages_[prev];
        pv.held = false;
        pv.pendingRelease = true;
        pv.releaseSlot = slotIdx;
        pv.releaseGeneration = slot.generation;
    }
    vi.held = true;
    if (dumping_) {
        // Written in presentation order.
        if (dev_.config.extrapolate) {
            dumpFrame(slot, false, 1.f);
            if (interpolate) dumpFrame(slot, true, 1.f + 1.f / mult);
        } else {
            if (interpolate) dumpFrame(slot, true, 1.f / mult);
            dumpFrame(slot, false, 1.f);
        }
    }

    // Hand the presentation over to the worker.
    Job job;
    job.parity = parity_;
    job.source = imageIndex;
    job.frames = interpolate ? mult : 1;
    job.generated = interpolate;
    job.realFirst = dev_.config.extrapolate;
    job.start = t0;
    job.intervalMs = frameIntervalMs_;
    if (info.swapchainCount == 1) {
        if (auto* pid = findChain<VkPresentIdKHR>(info.pNext, VK_STRUCTURE_TYPE_PRESENT_ID_KHR))
            if (pid->pPresentIds) job.presentId = pid->pPresentIds[0];
        if (auto* pf = findChain<VkSwapchainPresentFenceInfoEXT>(info.pNext, VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT))
            if (pf->pFences) job.presentFence = pf->pFences[0];
        if (auto* pm = findChain<VkSwapchainPresentModeInfoEXT>(info.pNext, VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT))
            if (pm->pPresentModes) {
                job.presentMode = pm->pPresentModes[0];
                presentMode_ = pm->pPresentModes[0];  // the pacing logic follows the switch
            }
    }
    {
        std::lock_guard<std::mutex> wlock(workerMutex_);
        jobs_.push_back(job);
        ++unsubmitted_;
    }
    workerCv_.notify_one();

    prevIndex_ = static_cast<int>(imageIndex);
    parity_ ^= 1;

    if (dev_.config.stats) {
        statsAppFrames_ += 1;
        statsCpuMs_ += std::chrono::duration<double, std::milli>(clock::now() - t0).count();
        const auto now = clock::now();
        const double elapsed = std::chrono::duration<double>(now - statsStart_).count();
        if (elapsed >= dev_.config.statsInterval) {
            const uint64_t out = statsOutFrames_.exchange(0);
            const uint64_t delayUs = statsRealDelayUs_.exchange(0);
            const uint64_t skipped = statsSkipped_.exchange(0);
            char skip[64] = "";
            if (skipped) snprintf(skip, sizeof(skip), ", %llu predicted frame(s) skipped", static_cast<unsigned long long>(skipped));
            BDEX_INFO("game %.1f fps -> output %.1f fps (x%.2f), present call %.2f ms, real frame delayed %.1f ms%s%s",
                      statsAppFrames_ / elapsed, out / elapsed, statsAppFrames_ ? double(out) / statsAppFrames_ : 0.0,
                      statsAppFrames_ ? statsCpuMs_ / statsAppFrames_ : 0.0,
                      statsAppFrames_ ? delayUs / 1000.0 / statsAppFrames_ : 0.0, skip, fg_->profileReport().c_str());
            statsStart_ = now;
            statsAppFrames_ = 0;
            statsCpuMs_ = 0;
        }
    }
    return pending;  // VK_SUCCESS or VK_SUBOPTIMAL_KHR from an earlier real present
}

// ---------------------------------------------------------------------------
// Worker side
// ---------------------------------------------------------------------------

void VirtualSwapchain::workerLoop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> wlock(workerMutex_);
            workerCv_.wait(wlock, [this] { return !jobs_.empty() || stop_; });
            if (jobs_.empty()) return;  // stop requested and nothing left
            job = jobs_.front();
            jobs_.pop_front();
            running_ = true;
        }
        runJob(job);
        {
            std::lock_guard<std::mutex> wlock(workerMutex_);
            running_ = false;
        }
        idleCv_.notify_all();
    }
}

void VirtualSwapchain::runJob(const Job& job) {
    bool submitted = false;
    std::function<void()> markSubmitted = [&] {
        if (submitted) return;
        submitted = true;
        {
            std::lock_guard<std::mutex> wlock(workerMutex_);
            --unsubmitted_;
        }
        idleCv_.notify_all();
    };
    for (int i = 0; i < job.frames; ++i) {
        VkResult r = presentOne(job, i, i == job.frames - 1 ? &markSubmitted : nullptr);
        if (r == VK_EVENT_RESET) {  // predicted frame skipped: a newer real frame is waiting
            statsSkipped_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        if (r < 0) {
            markSubmitted();
            std::lock_guard<std::mutex> wlock(workerMutex_);
            pendingResult_ = r;
            return;
        }
        if (r == VK_SUBOPTIMAL_KHR) {
            std::lock_guard<std::mutex> wlock(workerMutex_);
            if (pendingResult_ == VK_SUCCESS) pendingResult_ = r;
        }
        statsOutFrames_.fetch_add(1, std::memory_order_relaxed);
    }
    markSubmitted();
}

// Presents frame `index` of the job. Interpolation: generated frames first,
// the real one last. Extrapolation: the real frame first, then the
// predicted ones.
VkResult VirtualSwapchain::presentOne(const Job& job, int index, const std::function<void()>* onSubmitted) {
    const bool real = job.realFirst ? index == 0 : index == job.frames - 1;
    const uint32_t output = job.realFirst ? static_cast<uint32_t>(index - 1) : static_cast<uint32_t>(index);

    // With FIFO the presentation engine paces the frames; otherwise spread
    // them evenly over the game's frame interval. A predicted frame is
    // obsolete as soon as the next real one has arrived: skip it instead of
    // delaying the real frame behind it.
    if (index > 0 && dev_.config.pacing && presentMode_ != VK_PRESENT_MODE_FIFO_KHR &&
        presentMode_ != VK_PRESENT_MODE_FIFO_RELAXED_KHR) {
        const auto target = job.start + std::chrono::duration_cast<clock::duration>(
                                            std::chrono::duration<double, std::milli>(job.intervalMs * index / job.frames));
        std::unique_lock<std::mutex> wlock(workerMutex_);
        if (job.realFirst) {
            if (workerCv_.wait_until(wlock, target, [this] { return !jobs_.empty() || stop_; })) return VK_EVENT_RESET;
        } else {
            wlock.unlock();
            std::this_thread::sleep_until(target);
        }
    } else if (!real && job.realFirst) {
        std::lock_guard<std::mutex> wlock(workerMutex_);
        if (!jobs_.empty()) return VK_EVENT_RESET;
    }

    FrameSlot& slot = nextSlot(workerSlots_, workerCursor_);
    uint32_t ri = 0;
    VkResult r = dev_.vt.AcquireNextImageKHR(dev_.device, real_, UINT64_MAX, slot.acquireSem, VK_NULL_HANDLE, &ri);
    if (r < 0) return r;
    const bool suboptimal = (r == VK_SUBOPTIMAL_KHR);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dev_.vt.BeginCommandBuffer(slot.cmd, &bi);
    const uint32_t profSlot = static_cast<uint32_t>(slots_.size() + (&slot - workerSlots_.data()));
    fg_->profileBegin(slot.cmd, profSlot);
    if (real) fg_->recordCopySource(slot.cmd, job.source, realImages_[ri].image);
    else fg_->recordCopyOutput(slot.cmd, output, realImages_[ri].image);
    fg_->profileMark(slot.cmd, profSlot, FrameGen::StageCopy);
    dev_.vt.EndCommandBuffer(slot.cmd);

    // The copy reads images written by the synthesis submission, which
    // precedes it on the same queue: queue order is the only dependency.
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &slot.acquireSem;
    si.pWaitDstStageMask = &stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &slot.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &realImages_[ri].readySem;

    VkPresentIdKHR presentId{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
    VkSwapchainPresentFenceInfoEXT presentFence{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
    VkSwapchainPresentModeInfoEXT presentMode{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT};
    const VkPresentModeKHR presentModeValue = static_cast<VkPresentModeKHR>(job.presentMode);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &realImages_[ri].readySem;
    pi.swapchainCount = 1;
    pi.pSwapchains = &real_;
    pi.pImageIndices = &ri;
    const void** tail = &pi.pNext;
    if (real && job.presentId) {
        presentId.swapchainCount = 1;
        presentId.pPresentIds = &job.presentId;
        *tail = &presentId;
        tail = &presentId.pNext;
    }
    if (real && job.presentFence) {
        presentFence.swapchainCount = 1;
        presentFence.pFences = &job.presentFence;
        *tail = &presentFence;
        tail = &presentFence.pNext;
    }
    if (job.presentMode >= 0) {
        presentMode.swapchainCount = 1;
        presentMode.pPresentModes = &presentModeValue;
        *tail = &presentMode;
        tail = &presentMode.pNext;
    }

    std::lock_guard<std::mutex> qlock(dev_.queueMutex(dev_.queue));
    r = dev_.vt.QueueSubmit(dev_.queue, 1, &si, slot.fence);
    if (r < 0) return r;
    slot.submitted = true;
    if (onSubmitted) (*onSubmitted)();
    if (real)
        statsRealDelayUs_.fetch_add(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - job.start).count()),
            std::memory_order_relaxed);
    r = dev_.vt.QueuePresentKHR(dev_.queue, &pi);
    if (r == VK_SUCCESS && suboptimal) r = VK_SUBOPTIMAL_KHR;
    return r;
}

} // namespace bdex
