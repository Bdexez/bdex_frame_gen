#include "swapchain.h"
#include "log.h"
#include "vk_util.h"

#include <algorithm>
#include <thread>

namespace bdex {

namespace {

constexpr uint32_t kFrameSlots = 6;

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
        createVirtualImages(appInfo);
        createSlots();
        if (dev_.config.enabled && dev_.config.debug != Config::Debug::Passthrough && dev_.config.multiplier > 1) {
            try {
                fg_ = std::make_unique<FrameGen>(dev_, format_, extent_);
                generating_ = true;
            } catch (const VkError& e) {
                BDEX_WARN("frame generation unavailable for this swapchain (%s); passing frames through", e.what());
            }
        }
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
    }
    presentMode_ = ci.presentMode;

    VkSurfaceCapabilitiesKHR caps{};
    if (dev_.inst->vt.GetPhysicalDeviceSurfaceCapabilitiesKHR(dev_.physDev, appInfo.surface, &caps) == VK_SUCCESS) {
        // One extra image gives the generated frame and the real frame their own buffers.
        uint32_t want = std::max(appInfo.minImageCount, 3u);
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

void VirtualSwapchain::createVirtualImages(const VkSwapchainCreateInfoKHR& appInfo) {
    uint32_t count = std::max(appInfo.minImageCount, 3u);
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

    virtualImages_.resize(count);
    for (auto& vi : virtualImages_) {
        vi.img = dev_.createImage(format_, extent_, appInfo.imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, false, flags, next, &families);
    }
}

void VirtualSwapchain::createSlots() {
    slots_.resize(kFrameSlots);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = dev_.cmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    for (auto& s : slots_) {
        VK_CHECK(dev_.vt.AllocateCommandBuffers(dev_.device, &ai, &s.cmd));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(dev_.vt.CreateFence(dev_.device, &fi, nullptr, &s.fence));
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(dev_.vt.CreateSemaphore(dev_.device, &si, nullptr, &s.acquireSem));
    }
}

void VirtualSwapchain::waitIdle() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& s : slots_) {
        if (s.submitted) {
            dev_.vt.WaitForFences(dev_.device, 1, &s.fence, VK_TRUE, UINT64_MAX);
        }
    }
    // Acquire semaphores that were signalled by a real acquire but never
    // waited on cannot be reclaimed without a queue wait.
    std::lock_guard<std::mutex> qlock(dev_.queueMutex(dev_.queue));
    dev_.vt.QueueWaitIdle(dev_.queue);
}

void VirtualSwapchain::destroyAll() {
    auto& vt = dev_.vt;
    VkDevice d = dev_.device;
    for (auto& s : slots_) {
        if (s.submitted) vt.WaitForFences(d, 1, &s.fence, VK_TRUE, UINT64_MAX);
    }
    if (dev_.queue) {
        std::lock_guard<std::mutex> qlock(dev_.queueMutex(dev_.queue));
        vt.QueueWaitIdle(dev_.queue);
    }
    fg_.reset();
    for (auto& s : slots_) {
        if (s.cmd) vt.FreeCommandBuffers(d, dev_.cmdPool, 1, &s.cmd);
        if (s.fence) vt.DestroyFence(d, s.fence, nullptr);
        if (s.acquireSem) vt.DestroySemaphore(d, s.acquireSem, nullptr);
    }
    slots_.clear();
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

    if (pendingResult_ == VK_ERROR_OUT_OF_DATE_KHR) return VK_ERROR_OUT_OF_DATE_KHR;

    const uint32_t n = static_cast<uint32_t>(virtualImages_.size());
    int chosen = -1;
    for (uint32_t k = 0; k < n; ++k) {
        uint32_t i = (acquireCursor_ + k) % n;
        if (!virtualImages_[i].acquired) { chosen = static_cast<int>(i); break; }
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
    VkResult r = pendingResult_;
    pendingResult_ = VK_SUCCESS;
    return r;
}

VkResult VirtualSwapchain::release(uint32_t count, const uint32_t* indices) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t i = 0; i < count; ++i)
        if (indices[i] < virtualImages_.size()) virtualImages_[indices[i]].acquired = false;
    return VK_SUCCESS;
}

VirtualSwapchain::FrameSlot& VirtualSwapchain::nextSlot() {
    FrameSlot& s = slots_[slotCursor_];
    slotCursor_ = (slotCursor_ + 1) % slots_.size();
    if (s.submitted) {
        dev_.vt.WaitForFences(dev_.device, 1, &s.fence, VK_TRUE, UINT64_MAX);
        dev_.vt.ResetFences(dev_.device, 1, &s.fence);
        s.submitted = false;
        if (fg_) fg_->profileCollect(static_cast<uint32_t>(&s - slots_.data()));
    }
    ++s.generation;
    dev_.vt.ResetCommandBuffer(s.cmd, 0);
    return s;
}

VkResult VirtualSwapchain::acquireReal(FrameSlot& slot, uint32_t* realIndex) {
    VkResult r = dev_.vt.AcquireNextImageKHR(dev_.device, real_, UINT64_MAX, slot.acquireSem, VK_NULL_HANDLE, realIndex);
    if (r == VK_SUBOPTIMAL_KHR) {
        if (pendingResult_ == VK_SUCCESS) pendingResult_ = VK_SUBOPTIMAL_KHR;
        r = VK_SUCCESS;
    }
    return r;
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

VkResult VirtualSwapchain::submitAndPresent(FrameSlot& slot, uint32_t realIndex, const VkSemaphore* appWaits,
                                            uint32_t appWaitCount, const void* presentNext) {
    std::vector<VkSemaphore> waits(appWaits, appWaits + appWaitCount);
    waits.push_back(slot.acquireSem);
    std::vector<VkPipelineStageFlags> stages(waits.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = static_cast<uint32_t>(waits.size());
    si.pWaitSemaphores = waits.data();
    si.pWaitDstStageMask = stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &slot.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &realImages_[realIndex].readySem;

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.pNext = presentNext;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &realImages_[realIndex].readySem;
    pi.swapchainCount = 1;
    pi.pSwapchains = &real_;
    pi.pImageIndices = &realIndex;

    std::lock_guard<std::mutex> qlock(dev_.queueMutex(dev_.queue));
    VkResult r = dev_.vt.QueueSubmit(dev_.queue, 1, &si, slot.fence);
    if (r < 0) return r;
    slot.submitted = true;
    return dev_.vt.QueuePresentKHR(dev_.queue, &pi);
}

void VirtualSwapchain::paceBeforeNextPresent(clock::time_point frameStart, int generatedIndex, int total) {
    // With FIFO the display refresh paces the presents. Otherwise spread the
    // presents evenly across the measured game frame interval, starting from
    // the moment the game presented this frame.
    if (!dev_.config.pacing || presentMode_ == VK_PRESENT_MODE_FIFO_KHR || presentMode_ == VK_PRESENT_MODE_FIFO_RELAXED_KHR)
        return;
    const auto target = frameStart + std::chrono::duration_cast<clock::duration>(
                                         std::chrono::duration<double, std::milli>(frameIntervalMs_ * generatedIndex / total));
    const auto now = clock::now();
    if (target > now) std::this_thread::sleep_until(target);
}

void VirtualSwapchain::updateStats(int presented, double cpuMs) {
    if (!dev_.config.stats) return;
    statsAppFrames_ += 1;
    statsOutFrames_ += presented;
    statsCpuMs_ += cpuMs;
    const auto now = clock::now();
    const double elapsed = std::chrono::duration<double>(now - statsStart_).count();
    if (elapsed >= dev_.config.statsInterval) {
        BDEX_INFO("game %.1f fps -> output %.1f fps (x%.2f), present call %.2f ms avg%s", statsAppFrames_ / elapsed,
                  statsOutFrames_ / elapsed, statsAppFrames_ ? double(statsOutFrames_) / statsAppFrames_ : 0.0,
                  statsAppFrames_ ? statsCpuMs_ / statsAppFrames_ : 0.0, fg_ ? fg_->profileReport().c_str() : "");
        statsStart_ = now;
        statsAppFrames_ = statsOutFrames_ = 0;
        statsCpuMs_ = 0;
    }
}

void VirtualSwapchain::dumpFrame(FrameSlot& slot, bool generated, float t) {
    if (dumpCount_ >= dev_.config.dumpFrames) { dumping_ = false; return; }
    dev_.vt.WaitForFences(dev_.device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
    char name[64];
    snprintf(name, sizeof(name), "/sc%u_%ux%u_%04d_%s_t%.2f.ppm", dumpId_, extent_.width, extent_.height, dumpCount_,
             generated ? "gen" : "real", t);
    const std::string path = dev_.config.dumpDir + name;
    if (fg_->writeDump(path)) BDEX_DBG("dumped %s", path.c_str());
    else BDEX_WARN("could not write %s", path.c_str());
    ++dumpCount_;
}

VkResult VirtualSwapchain::present(uint32_t imageIndex, const VkPresentInfoKHR& info, bool consumeWaitSemaphores) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto t0 = clock::now();
    const VkSemaphore* appWaits = consumeWaitSemaphores ? info.pWaitSemaphores : nullptr;
    const uint32_t appWaitCount = consumeWaitSemaphores ? info.waitSemaphoreCount : 0;
    // pNext structures carry per-swapchain arrays sized for the application's
    // present; they can only be forwarded when it presented this swapchain alone.
    const void* presentNext = info.swapchainCount == 1 ? info.pNext : nullptr;

    if (imageIndex >= virtualImages_.size()) return VK_ERROR_OUT_OF_DATE_KHR;
    VirtualImage& vi = virtualImages_[imageIndex];
    vi.acquired = false;

    // Frame interval estimate (EMA) for pacing.
    if (lastAppPresent_.time_since_epoch().count() != 0) {
        double dt = std::chrono::duration<double, std::milli>(t0 - lastAppPresent_).count();
        if (dt > 0.5 && dt < 500.0) frameIntervalMs_ = frameIntervalMs_ * 0.8 + dt * 0.2;
    }

    const int mult = generating_ ? std::max(1, dev_.config.multiplier) : 1;
    int presented = 0;
    VkResult result = VK_SUCCESS;

    auto fail = [&](VkResult r) {
        // The application's semaphores must still be consumed (a real present
        // would have waited on them even when it fails).
        if (appWaitCount) consumeSemaphores(appWaits, appWaitCount);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) pendingResult_ = r;
        return r;
    };

    // First real present: analyse the new frame and either generate the first
    // intermediate frame (when we have a previous one) or copy the frame.
    {
        FrameSlot& slot = nextSlot();
        uint32_t ri = 0;
        VkResult r = acquireReal(slot, &ri);
        if (r < 0) return fail(r);

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        dev_.vt.BeginCommandBuffer(slot.cmd, &bi);
        const bool interpolate = generating_ && havePrevious_ && mult > 1;
        if (generating_) {
            const uint32_t slotIdx = static_cast<uint32_t>(&slot - slots_.data());
            fg_->profileBegin(slot.cmd, slotIdx);
            fg_->recordAnalysis(slot.cmd, parity_, vi.img.image);
            fg_->profileMark(slot.cmd, slotIdx, FrameGen::StageAnalysis);
            if (interpolate) {
                fg_->recordFlow(slot.cmd, parity_);
                fg_->profileMark(slot.cmd, slotIdx, FrameGen::StageFlow);
                fg_->recordInterpolate(slot.cmd, parity_, 1.f / mult, realImages_[ri].image);
                fg_->profileMark(slot.cmd, slotIdx, FrameGen::StageInterp);
            } else {
                fg_->recordCopyHistory(slot.cmd, parity_, realImages_[ri].image);
                fg_->profileMark(slot.cmd, slotIdx, FrameGen::StageCopy);
            }
            if (dumping_) fg_->recordDump(slot.cmd, interpolate, parity_);
        } else {
            // No generation: straight copy of the application's image.
            FrameGen::recordCopyDirect(dev_, slot.cmd, vi.img.image, realImages_[ri].image, extent_);
        }
        dev_.vt.EndCommandBuffer(slot.cmd);

        vi.pendingRelease = true;
        vi.releaseSlot = static_cast<uint32_t>(&slot - slots_.data());
        vi.releaseGeneration = slot.generation;

        r = submitAndPresent(slot, ri, appWaits, appWaitCount, interpolate ? nullptr : presentNext);
        if (r < 0) {
            if (!slot.submitted) return fail(r);
            if (r == VK_ERROR_OUT_OF_DATE_KHR) pendingResult_ = r;
            return r;
        }
        if (r == VK_SUBOPTIMAL_KHR) result = r;
        ++presented;
        if (dumping_) dumpFrame(slot, interpolate, interpolate ? 1.f / mult : 1.f);

        if (!interpolate) {
            havePrevious_ = generating_;
            parity_ ^= 1;
            lastAppPresent_ = t0;
            updateStats(presented, std::chrono::duration<double, std::milli>(clock::now() - t0).count());
            return result;
        }
    }

    // Remaining generated frames, then the real one.
    for (int i = 2; i <= mult; ++i) {
        paceBeforeNextPresent(t0, i - 1, mult);
        FrameSlot& slot = nextSlot();
        uint32_t ri = 0;
        VkResult r = acquireReal(slot, &ri);
        if (r < 0) { pendingResult_ = r; return r; }

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        dev_.vt.BeginCommandBuffer(slot.cmd, &bi);
        const bool last = (i == mult);
        const uint32_t slotIdx = static_cast<uint32_t>(&slot - slots_.data());
        fg_->profileBegin(slot.cmd, slotIdx);
        if (last) {
            fg_->recordCopyHistory(slot.cmd, parity_, realImages_[ri].image);
            fg_->profileMark(slot.cmd, slotIdx, FrameGen::StageCopy);
        } else {
            fg_->recordInterpolate(slot.cmd, parity_, float(i) / mult, realImages_[ri].image);
            fg_->profileMark(slot.cmd, slotIdx, FrameGen::StageInterp);
        }
        if (dumping_) fg_->recordDump(slot.cmd, !last, parity_);
        dev_.vt.EndCommandBuffer(slot.cmd);

        r = submitAndPresent(slot, ri, nullptr, 0, last ? presentNext : nullptr);
        if (r < 0) { pendingResult_ = r; return r; }
        if (r == VK_SUBOPTIMAL_KHR) result = r;
        ++presented;
        if (dumping_) dumpFrame(slot, !last, float(i) / mult);
    }

    parity_ ^= 1;
    lastAppPresent_ = t0;
    updateStats(presented, std::chrono::duration<double, std::milli>(clock::now() - t0).count());
    return result;
}

} // namespace bdex
