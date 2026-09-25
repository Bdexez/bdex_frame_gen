#include "swapchain.h"
#include "online_tune.h"
#include "status.h"
#include "log.h"
#include "vk_util.h"

#include <algorithm>
#include <cmath>
#include <cstring>
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
    dev_.snapshotConfig(cfg_, cfgVersion_);
    format_ = appInfo.imageFormat;
    colorSpace_ = appInfo.imageColorSpace;
    extent_ = appInfo.imageExtent;
    try {
        createReal(appInfo);
        const bool wantGeneration = cfg_.enabled && cfg_.debug != Config::Debug::Passthrough &&
                                    cfg_.multiplier > 1 && !throwaway_;
        createVirtualImages(appInfo, wantGeneration);
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        // The synthesis runs on the async compute family when there is one.
        pci.queueFamilyIndex = dev_.asyncCompute ? dev_.computeFamily : dev_.queueFamily;
        VK_CHECK(dev_.vt.CreateCommandPool(dev_.device, &pci, nullptr, &appPool_));
        pci.queueFamilyIndex = dev_.queueFamily;
        VK_CHECK(dev_.vt.CreateCommandPool(dev_.device, &pci, nullptr, &workerPool_));
        createSlots(slots_, kAppSlots, false, appPool_, dev_.asyncCompute, false);
        createSlots(workerSlots_, kWorkerSlots, true, workerPool_, false, dev_.asyncCompute);
        generating_ = wantGeneration;
        fg_ = makeFrameGen(generating_);
        worker_ = std::thread([this] { workerLoop(); });
        feedback_ = dev_.presentFeedback && !throwaway_;
        if (feedback_) fbThread_ = std::thread([this] { feedbackLoop(); });
    } catch (...) {
        destroyAll();
        throw;
    }
    statsStart_ = clock::now();
    dumping_ = !cfg_.dumpDir.empty();
    publishStatus(cfg_.multiplier);
    static uint32_t nextDumpId = 0;
    dumpId_ = nextDumpId++;
    BDEX_INFO("swapchain %ux%u format %d: %zu virtual images, %zu real images, present mode %s, generation %s",
              extent_.width, extent_.height, static_cast<int>(format_), virtualImages_.size(), realImages_.size(),
              presentModeName(presentMode_), generating_ ? "on" : "off");
}

VirtualSwapchain::~VirtualSwapchain() {
    statusRemove(statusKey(this));
    destroyAll();
}

void VirtualSwapchain::publishStatus(int multiplier) {
    StatusEntry e;
    e.kind = "swapchain";
    const bool on = generating_ && !cfg_.paused;
    e.state = on ? "generating" : generating_ ? "paused" : "pass-through";
    if (!generating_) e.reason = genFailReason_.empty() ? "frame generation off for this swapchain" : genFailReason_;
    e.mode = cfg_.extrapolate ? "extrapolate" : "interpolate";
    e.multiplier = on ? multiplier : 1;
    e.gameFps = frameIntervalMs_ > 0.05 ? 1000.0 / frameIntervalMs_ : 0.0;
    e.outFps = e.gameFps * e.multiplier;
    e.refreshHz = displayHz();
    e.latencyMs = displayLatencyMs();
    e.budgetLevel = budget_.level();
    e.width = displayExtent_.width;
    e.height = displayExtent_.height;
    e.device = dev_.props.deviceName;
    if (fg_) e.passes = fg_->passNames();
    {
        std::lock_guard<std::mutex> lock(dev_.onlineTuneMutex);
        if (dev_.onlineTune) e.tune = dev_.onlineTune->status();
    }
    statusSet(statusKey(this), e);
}

// Builds the output stage for the current render / display extents. On entry
// `generating` says whether frame generation is wanted; on return, whether it
// is available (a pass-through instance is built otherwise).
std::unique_ptr<FrameGen> VirtualSwapchain::makeFrameGen(bool& generating) {
    std::vector<VkImage> sources;
    for (auto& vi : virtualImages_) sources.push_back(vi.img.image);
    if (generating) {
        try {
            return std::make_unique<FrameGen>(dev_, format_, extent_, displayExtent_,
                                              static_cast<uint32_t>(cfg_.multiplier - 1), sources, colorSpace_,
                                              flowScaleFactor_, &cfg_);
        } catch (const VkError& e) {
            BDEX_WARN("frame generation unavailable for this swapchain (%s); passing frames through", e.what());
            genFailReason_ = std::string("frame generation unavailable: ") + e.what();
            generating = false;
        }
    }
    return std::make_unique<FrameGen>(dev_, format_, extent_, displayExtent_, 0, sources, colorSpace_, 1, &cfg_);
}

void VirtualSwapchain::createReal(const VkSwapchainCreateInfoKHR& appInfo) {
    VkSwapchainCreateInfoKHR ci = appInfo;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.queueFamilyIndexCount = 0;
    ci.pQueueFamilyIndices = nullptr;

    // The pNext chain is forwarded unchanged (format lists, scaling, present
    // modes...): it describes the surface-side behaviour which stays the same.
    if (cfg_.presentMode >= 0 &&
        findChain<VkBaseInStructure>(appInfo.pNext, VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT)) {
        BDEX_WARN("application declares its present modes (VK_EXT_swapchain_maintenance1); ignoring present_mode override");
    } else if (cfg_.presentMode >= 0) {
        auto mode = static_cast<VkPresentModeKHR>(cfg_.presentMode);
        if (presentModeSupported(dev_, appInfo.surface, mode)) ci.presentMode = mode;
        else BDEX_WARN("present mode %s not supported by the surface, keeping %s", presentModeName(mode), presentModeName(appInfo.presentMode));
    } else if (cfg_.presentMode == -2 &&
               (ci.presentMode == VK_PRESENT_MODE_FIFO_KHR || ci.presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR)) {
        // Never present the real swapchain in FIFO: our worker presents
        // asynchronously, and a FIFO present hard-hangs Direct3D translation
        // layers (DXVK/VKD3D) after one frame. FIFO also queues our extra
        // frames in the presentation engine (2-3 refreshes of latency). Prefer
        // mailbox (no tearing), fall back to immediate; keep our own pacing.
        VkPresentModeKHR want = VK_PRESENT_MODE_MAX_ENUM_KHR;
        if (presentModeSupported(dev_, appInfo.surface, VK_PRESENT_MODE_MAILBOX_KHR)) want = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (presentModeSupported(dev_, appInfo.surface, VK_PRESENT_MODE_IMMEDIATE_KHR)) want = VK_PRESENT_MODE_IMMEDIATE_KHR;
        if (want != VK_PRESENT_MODE_MAX_ENUM_KHR) {
            BDEX_INFO("presenting with %s instead of %s (present_mode=app keeps the game's mode)",
                      presentModeName(want), presentModeName(ci.presentMode));
            ci.presentMode = want;
        } else {
            BDEX_WARN("neither mailbox nor immediate present modes are available; keeping %s (may hang some games)",
                      presentModeName(ci.presentMode));
        }
    }
    presentMode_ = ci.presentMode;

    // The application's create info carries the render extent (reduced by our
    // surface-capabilities hook when upscaling); the real swapchain must use
    // the true display extent, which this direct query returns unmodified.
    displayExtent_ = extent_;
    surface_ = appInfo.surface;
    VkSurfaceCapabilitiesKHR caps{};
    if (dev_.inst->vt.GetPhysicalDeviceSurfaceCapabilitiesKHR(dev_.physDev, appInfo.surface, &caps) == VK_SUCCESS) {
        const bool upscale = cfg_.enabled && cfg_.renderScale < 0.999f;
        if (upscale && caps.currentExtent.width != UINT32_MAX &&
            (caps.currentExtent.width != extent_.width || caps.currentExtent.height != extent_.height)) {
            // X11 / XWayland: the caps hook already handed the app a reduced
            // size; the real swapchain presents at the true (unmodified) one.
            displayExtent_ = caps.currentExtent;
            upscaling_ = true;
            ci.imageExtent = displayExtent_;
        } else if (upscale && caps.currentExtent.width == UINT32_MAX) {
            // Native Wayland: the surface never reports its size, so we learn it
            // here (the app just asked for the true window size) and bounce the
            // app through one recreation; the caps hook then reports a reduced
            // currentExtent and the recreated swapchain renders smaller.
            std::lock_guard<std::mutex> lk(dev_.inst->surfMutex);
            WaylandSurfaceState& st = dev_.inst->waylandSurfaces[appInfo.surface];
            if (!st.reduce) {
                st.displayExtent = extent_;   // the true window size
                st.reduce = true;
                throwaway_ = true;            // this swapchain's first acquire returns OUT_OF_DATE
            } else {
                displayExtent_ = st.displayExtent;   // present at the full window size
                upscaling_ = true;
                ci.imageExtent = displayExtent_;
            }
        }
        // We present `multiplier` frames per game frame: with too few images
        // the acquire of the real frame blocks behind the generated ones
        // still queued in the presentation engine, which delays it.
        const uint32_t mult = static_cast<uint32_t>(std::max(1, cfg_.multiplier));
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
    realInfo_ = ci;
    realInfo_.pNext = nullptr;
    realInfo_.oldSwapchain = VK_NULL_HANDLE;
    createRealImages();
}

void VirtualSwapchain::createRealImages() {
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

void VirtualSwapchain::destroyRealImages(std::vector<RealImage>& images) {
    for (auto& ri : images)
        if (ri.readySem) dev_.vt.DestroySemaphore(dev_.device, ri.readySem, nullptr);
    images.clear();
}

// The surface can change size underneath the real swapchain without the
// application's swapchain being involved: on X11 / Xwayland, Wine applies an
// emulated display mode by resizing the window after the game created its
// swapchain. The driver then reports VK_SUBOPTIMAL_KHR and presents our
// images cropped (or padded). The application is not told (see runJob), so
// we re-create the real swapchain at the new extent ourselves, together with
// the output stage that produces frames at display size. The application's
// virtual images are untouched; only the previous frame's analysis is lost.
void VirtualSwapchain::followSurface() {
    VkSurfaceCapabilitiesKHR caps{};
    if (dev_.inst->vt.GetPhysicalDeviceSurfaceCapabilitiesKHR(dev_.physDev, realInfo_.surface, &caps) != VK_SUCCESS) return;
    const VkExtent2D cur = caps.currentExtent;
    if (cur.width == UINT32_MAX || cur.width == 0 || cur.height == 0) return;
    if (cur.width == displayExtent_.width && cur.height == displayExtent_.height) return;

    {
        std::unique_lock<std::mutex> wlock(workerMutex_);
        waitWorkerIdle(wlock);
    }
    waitQueuesIdle();
    // The feedback thread must not wait on the swapchain being replaced.
    struct FeedbackPause {
        VirtualSwapchain& s;
        explicit FeedbackPause(VirtualSwapchain& v) : s(v) { s.pauseFeedback(); }
        ~FeedbackPause() { s.resumeFeedback(); }
    } feedbackPause(*this);

    const VkExtent2D old = displayExtent_;
    displayExtent_ = cur;
    upscaling_ = (cur.width != extent_.width || cur.height != extent_.height);
    bool generating = generating_;
    std::unique_ptr<FrameGen> fg;
    try {
        fg = makeFrameGen(generating);
    } catch (const VkError& e) {
        BDEX_WARN("surface resized to %ux%u but the output stage could not be rebuilt (%s)", cur.width, cur.height, e.what());
        displayExtent_ = old;
        upscaling_ = (old.width != extent_.width || old.height != extent_.height);
        std::lock_guard<std::mutex> wlock(workerMutex_);
        pendingResult_ = VK_ERROR_OUT_OF_DATE_KHR;  // let the application rebuild everything
        acquireCv_.notify_all();
        return;
    }

    VkSwapchainCreateInfoKHR ci = realInfo_;
    ci.imageExtent = cur;
    ci.oldSwapchain = real_;
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.minImageCount = std::max(ci.minImageCount, caps.minImageCount);
    VkSwapchainKHR fresh = VK_NULL_HANDLE;
    VkResult r = dev_.vt.CreateSwapchainKHR(dev_.device, &ci, nullptr, &fresh);
    if (r < 0) {
        // The old swapchain may have been retired by the failed call: the
        // application has to rebuild from scratch.
        BDEX_WARN("surface resized to %ux%u but the real swapchain could not be re-created: %s", cur.width, cur.height, vkResultName(r));
        displayExtent_ = old;
        upscaling_ = (old.width != extent_.width || old.height != extent_.height);
        std::lock_guard<std::mutex> wlock(workerMutex_);
        pendingResult_ = VK_ERROR_OUT_OF_DATE_KHR;
        acquireCv_.notify_all();
        return;
    }
    std::vector<RealImage> oldImages = std::move(realImages_);
    VkSwapchainKHR oldReal = real_;
    real_ = fresh;
    fg_ = std::move(fg);
    fg_->setQualityLevel(budget_.level());
    generating_ = generating;
    historyLost_ = true;
    try {
        createRealImages();
    } catch (const VkError& e) {
        BDEX_WARN("real swapchain images unavailable after resize (%s)", e.what());
        std::lock_guard<std::mutex> wlock(workerMutex_);
        pendingResult_ = VK_ERROR_OUT_OF_DATE_KHR;
        acquireCv_.notify_all();
    }
    destroyRealImages(oldImages);
    dev_.vt.DestroySwapchainKHR(dev_.device, oldReal, nullptr);
    BDEX_INFO("surface resized %ux%u -> %ux%u: real swapchain re-created (render %ux%u, %s)", old.width, old.height,
              cur.width, cur.height, extent_.width, extent_.height, upscaling_ ? "resampled" : "1:1");
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
    for (uint32_t f : dev_.layerFamilies())
        if (std::find(families.begin(), families.end(), f) == families.end()) families.push_back(f);
    std::sort(families.begin(), families.end());
    families.erase(std::unique(families.begin(), families.end()), families.end());

    // The application must receive images with every usage it requested.
    // A surface can support usages that an ordinary image of this format
    // cannot; in that case createImage fails and CreateSwapchainKHR falls
    // back to the driver's swapchain instead of silently changing the contract.
    VkImageUsageFlags usage = appInfo.imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (forGeneration || upscaling_ || cfg_.overlayMode > 0)
        usage |= VK_IMAGE_USAGE_SAMPLED_BIT;  // the upscale/HUD pass samples the source

    virtualImages_.resize(count);
    for (auto& vi : virtualImages_) {
        vi.img = dev_.createImage(format_, extent_, usage, false, flags, next, &families);
    }
}

void VirtualSwapchain::createSlots(std::vector<FrameSlot>& slots, uint32_t count, bool withSemaphore, VkCommandPool pool,
                                   bool asyncSynthesis, bool asyncCopy) {
    slots.resize(count);
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& s : slots) {
        VK_CHECK(dev_.allocateCommandBuffer(&s.cmd, pool));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(dev_.vt.CreateFence(dev_.device, &fi, nullptr, &s.fence));
        if (withSemaphore) VK_CHECK(dev_.vt.CreateSemaphore(dev_.device, &si, nullptr, &s.acquireSem));
        if (asyncSynthesis) {
            for (auto& sem : s.synthDone) VK_CHECK(dev_.vt.CreateSemaphore(dev_.device, &si, nullptr, &sem));
            VK_CHECK(dev_.vt.CreateSemaphore(dev_.device, &si, nullptr, &s.realIn));
            VK_CHECK(dev_.vt.CreateSemaphore(dev_.device, &si, nullptr, &s.synthIn));
        }
        if (asyncCopy) VK_CHECK(dev_.vt.CreateSemaphore(dev_.device, &si, nullptr, &s.copyDone));
    }
}

// Every queue the layer submits to (the async compute one too) is idle:
// before resources the GPU may still use are destroyed or rebuilt.
void VirtualSwapchain::waitQueuesIdle() {
    for (VkQueue q : {dev_.queue, dev_.asyncCompute ? dev_.computeQueue : VkQueue(VK_NULL_HANDLE)}) {
        if (!q) continue;
        std::lock_guard<std::mutex> qlock(dev_.queueMutex(q));
        dev_.vt.QueueWaitIdle(q);
    }
}

VkResult VirtualSwapchain::submitOn(VkQueue queue, const VkSubmitInfo& si, VkFence fence) {
    std::lock_guard<std::mutex> qlock(dev_.queueMutex(queue));
    return dev_.vt.QueueSubmit(queue, 1, &si, fence);
}

void VirtualSwapchain::destroySlots(std::vector<FrameSlot>& slots, VkCommandPool pool) {
    for (auto& s : slots) {
        if (s.submitted) dev_.vt.WaitForFences(dev_.device, 1, &s.fence, VK_TRUE, UINT64_MAX);
        if (s.cmd && pool) dev_.vt.FreeCommandBuffers(dev_.device, pool, 1, &s.cmd);
        if (s.fence) dev_.vt.DestroyFence(dev_.device, s.fence, nullptr);
        for (VkSemaphore sem : {s.acquireSem, s.realIn, s.synthIn, s.copyDone, s.synthDone[0], s.synthDone[1],
                                s.synthDone[2], s.synthDone[3]})
            if (sem) dev_.vt.DestroySemaphore(dev_.device, sem, nullptr);
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
    waitQueuesIdle();
}

void VirtualSwapchain::retire() {
    std::lock_guard<std::mutex> lock(mutex_);
    retired_ = true;
    acquireCv_.notify_all();
    std::unique_lock<std::mutex> wlock(workerMutex_);
    waitWorkerIdle(wlock);
}

void VirtualSwapchain::destroyAll() {
    auto& vt = dev_.vt;
    VkDevice d = dev_.device;
    if (fbThread_.joinable()) {  // it may be waiting on real_: stop it first
        {
            std::lock_guard<std::mutex> lock(fbMutex_);
            fbStop_ = true;
        }
        fbPaused_ = true;
        fbCv_.notify_all();
        fbThread_.join();
    }
    if (worker_.joinable()) {
        {
            std::lock_guard<std::mutex> wlock(workerMutex_);
            stop_ = true;
        }
        workerCv_.notify_all();
        worker_.join();
    }
    // Both queues idle before any slot's semaphores go: unfenced batches (the
    // semaphore-consuming ones) and the async compute queue may still refer
    // to them.
    waitQueuesIdle();
    destroySlots(slots_, appPool_);
    destroySlots(workerSlots_, workerPool_);
    if (appPool_) dev_.vt.DestroyCommandPool(d, appPool_, nullptr);
    if (workerPool_) dev_.vt.DestroyCommandPool(d, workerPool_, nullptr);
    appPool_ = workerPool_ = VK_NULL_HANDLE;
    if (dev_.queue) {
        // Acquire semaphores signalled by a real acquire but never waited on
        // cannot be reclaimed without a queue wait.
        std::lock_guard<std::mutex> qlock(dev_.queueMutex(dev_.queue));
        vt.QueueWaitIdle(dev_.queue);
    }
    fg_.reset();
    for (auto& vi : virtualImages_) dev_.destroyImage(vi.img);
    virtualImages_.clear();
    destroyRealImages(realImages_);
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

VkResult VirtualSwapchain::acquire(uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* index) {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool infinite = timeout == UINT64_MAX;
    const auto start = clock::now();
    const auto requested = std::chrono::duration_cast<clock::duration>(
        std::chrono::nanoseconds(std::min<uint64_t>(timeout, static_cast<uint64_t>(INT64_MAX))));
    const auto deadline = infinite ? clock::time_point::max() : start + std::min(requested, clock::time_point::max() - start);
    const uint32_t n = static_cast<uint32_t>(virtualImages_.size());
    int chosen = -1;
    VkResult pending = VK_SUCCESS;
    for (;;) {
        {
            std::lock_guard<std::mutex> wlock(workerMutex_);
            pending = pendingResult_;
        }
        if (pending < 0) return pending;
        if (retired_ || throwaway_) return VK_ERROR_OUT_OF_DATE_KHR;
        bool gpuPending = false;
        for (uint32_t k = 0; k < n; ++k) {
            uint32_t i = (acquireCursor_ + k) % n;
            VirtualImage& candidate = virtualImages_[i];
            if (candidate.acquired || candidate.held) continue;
            if (candidate.pendingRelease) {
                FrameSlot& s = slots_[candidate.releaseSlot];
                if (s.generation == candidate.releaseGeneration && s.submitted) {
                    VkResult ready = dev_.vt.WaitForFences(dev_.device, 1, &s.fence, VK_TRUE, 0);
                    if (ready == VK_TIMEOUT) { gpuPending = true; continue; }
                    if (ready < 0) return ready;
                }
                candidate.pendingRelease = false;
            }
            chosen = static_cast<int>(i);
            break;
        }
        if (chosen >= 0) break;
        if (timeout == 0) return VK_NOT_READY;
        if (!infinite && clock::now() >= deadline) return VK_TIMEOUT;
        // Fence completion has no CPU notification. Worker errors are also
        // published under workerMutex_, not mutex_, so a notification can
        // race with this wait. Recheck periodically even for UINT64_MAX.
        const auto poll = clock::now() + std::chrono::milliseconds(gpuPending ? 1 : 10);
        acquireCv_.wait_until(lock, std::min(deadline, poll));
    }
    VirtualImage& vi = virtualImages_[chosen];
    vi.acquired = true;
    acquireCursor_ = static_cast<uint32_t>(chosen) + 1;
    *index = static_cast<uint32_t>(chosen);

    if (semaphore || fence) {
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.signalSemaphoreCount = semaphore ? 1 : 0;
        si.pSignalSemaphores = &semaphore;
        VkResult r = dev_.submit(si, fence);
        if (r < 0) {
            vi.acquired = false;
            acquireCv_.notify_all();
            BDEX_ERR("failed to signal acquire semaphore: %s", vkResultName(r));
            return r;
        }
    }
    if (pending != VK_SUCCESS) {
        std::lock_guard<std::mutex> wlock(workerMutex_);
        if (pendingResult_ == pending) pendingResult_ = VK_SUCCESS;
    }
    return pending;
}

VkResult VirtualSwapchain::release(uint32_t count, const uint32_t* indices) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t i = 0; i < count; ++i)
        if (indices[i] < virtualImages_.size()) virtualImages_[indices[i]].acquired = false;
    acquireCv_.notify_all();
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

void VirtualSwapchain::dumpFrame(FrameSlot& slot, bool generated, float t, uint32_t output) {
    if (dumpCount_ >= cfg_.dumpFrames) { dumping_ = false; return; }
    dev_.vt.WaitForFences(dev_.device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
    char name[96];
    snprintf(name, sizeof(name), "/sc%u_%ux%u_%04d_%s_t%.2f.ppm", dumpId_, extent_.width, extent_.height, dumpCount_,
             generated ? "gen" : "real", t);
    const std::string path = cfg_.dumpDir + name;
    if (fg_->writeDump(path, generated ? output : fg_->outputCount())) BDEX_DBG("dumped %s", path.c_str());
    else BDEX_WARN("could not write %s", path.c_str());
    ++dumpCount_;
}

// ---------------------------------------------------------------------------
// Application side
// ---------------------------------------------------------------------------

// Feed one game frame interval into the HUD window and, a few times a second,
// recompute the 1% / 0.1% low fps (fps at the 99th / 99.9th percentile frame
// time over the window). Called under mutex_ from present().
void VirtualSwapchain::updateHudLows(double dtMs) {
    if (hudIntervals_.size() < kHudWindow) hudIntervals_.push_back(static_cast<float>(dtMs));
    else hudIntervals_[hudRingPos_] = static_cast<float>(dtMs);
    hudRingPos_ = (hudRingPos_ + 1) % kHudWindow;

    const auto now = clock::now();
    if (hudStatsAt_.time_since_epoch().count() != 0 &&
        std::chrono::duration<double>(now - hudStatsAt_).count() < 0.25)
        return;                              // recompute at most ~4x/second
    hudStatsAt_ = now;
    const size_t n = hudIntervals_.size();
    // Percentiles are defined from the first sample; early on the lows simply
    // track the current fps (better than showing 0) and diverge as the window
    // fills and stutters accumulate.
    std::vector<float> s = hudIntervals_;
    std::sort(s.begin(), s.end());           // ascending frame times
    auto lowFps = [&](double q) {
        size_t i = static_cast<size_t>(std::ceil(q * (n - 1)));
        float ft = s[std::min(i, n - 1)];
        return ft > 0.05f ? static_cast<int>(1000.0f / ft + 0.5f) : 0;
    };
    hudLow1_ = lowFps(0.99);
    hudLow01_ = lowFps(0.999);
}

// Frames presented per game frame. Fixed at `multiplier`, unless target_fps
// is set: then the multiplier follows target_fps x the game's frame interval
// (so 72 fps on a 144 Hz target gives x2, 50 fps gives x3, and x1 once the
// game reaches the target), capped at `multiplier`, which the output stage
// was sized for. A switch needs the ideal ratio to pass the halfway point by
// 0.15, so a game hovering on a boundary does not flip every frame.
int VirtualSwapchain::chooseMultiplier() {
    const int maxMult = std::max(1, cfg_.multiplier);
    if (cfg_.paused) return 1;
    // auto: the measured refresh rate; unknown (no feedback, VRR): fixed.
    const double target = cfg_.targetFps < 0.f ? displayHz() : double(cfg_.targetFps);
    if (target <= 0.0) return maxMult;
    const double ratio = target * frameIntervalMs_ / 1000.0;
    if (dynMult_ == 0) dynMult_ = std::clamp(static_cast<int>(ratio + 0.5), 1, maxMult);
    else if (ratio >= dynMult_ + 0.65) dynMult_ = std::min(maxMult, static_cast<int>(ratio + 0.5));
    else if (ratio <= dynMult_ - 0.65) dynMult_ = std::max(1, static_cast<int>(ratio + 0.5));
    return dynMult_;
}

// Frame-rate limiter (fps_limit), like DXVK_FRAME_RATE: the application's
// present blocks until its next slot, which paces the game itself. Frame
// generation works best from an even base rate at a divisor of the refresh
// rate (60 -> 120, 72 -> 144). Sleeps until ~1.5 ms before the deadline, then
// spins briefly for precision; after a late frame the schedule restarts from
// now instead of letting the game burst to catch up.
void VirtualSwapchain::limitFrameRate() {
    // auto: the refresh rate divided by the multiplier (60 on 120 Hz at x2),
    // once the present feedback knows it; no cap until then.
    const double fps = cfg_.fpsLimit < 0.f ? displayHz() / std::max(1, cfg_.multiplier)
                                                  : double(cfg_.fpsLimit);
    if (fps <= 0.0) return;
    std::lock_guard<std::mutex> lk(limitMutex_);
    const auto interval = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(1.0 / fps));
    auto now = clock::now();
    if (limitNext_ > now) {
        const auto coarse = limitNext_ - std::chrono::microseconds(1500);
        if (coarse > now) std::this_thread::sleep_until(coarse);
        while ((now = clock::now()) < limitNext_) std::this_thread::yield();
    }
    limitNext_ = (limitNext_ == clock::time_point{} || now - limitNext_ > interval) ? now + interval
                                                                                  : limitNext_ + interval;
}

// gpu_budget: about once a second, compare the generation's GPU time with the
// allowed share of the game's frame interval and move along the quality
// ladder (see GpuBudget). Levels 1-2 are per-frame overrides; the coarser
// flow levels rebuild the output stage (like a resize: one frame without
// generation). Called under mutex_ from present().
void VirtualSwapchain::applyGpuBudget() {
    const auto now = clock::now();
    if (budgetAt_.time_since_epoch().count() == 0) budgetAt_ = now;
    if (now - budgetAt_ < std::chrono::seconds(1)) return;
    budgetAt_ = now;
    const double gpuMs = fg_->takeGpuMs();
    const int base = cfg_.flowScaleFor(extent_.width, extent_.height);
    const int maxLevel = 2 + (base <= 2 ? 1 : 0) + (base <= 1 ? 1 : 0);  // flow divisor up to 4
    const int before = budget_.level();
    const int level = budget_.update(gpuMs, frameIntervalMs_, cfg_.gpuBudget, maxLevel);
    if (level == before) return;
    const int factor = level >= 4 ? 4 : level >= 3 ? 2 : 1;
    if (factor != flowScaleFactor_) {
        {
            std::unique_lock<std::mutex> wlock(workerMutex_);
            waitWorkerIdle(wlock);
        }
        waitQueuesIdle();
        const int oldFactor = flowScaleFactor_;
        flowScaleFactor_ = factor;
        bool generating = generating_;
        try {
            auto fg = makeFrameGen(generating);
            fg_ = std::move(fg);
            generating_ = generating;
            historyLost_ = true;
        } catch (const VkError& e) {
            BDEX_WARN("gpu budget: output stage not rebuilt (%s)", e.what());
            flowScaleFactor_ = oldFactor;
        }
    }
    fg_->setQualityLevel(level);
    BDEX_INFO("gpu budget: generation %.2f ms for a %.1f%% budget of %.1f ms: quality level %d -> %d (flow 1/%d)",
              gpuMs, 100.0 * cfg_.gpuBudget, frameIntervalMs_, before, level, fg_->flowScale());
}

VkResult VirtualSwapchain::present(uint32_t imageIndex, const VkPresentInfoKHR& info, bool consumeWaitSemaphores) {
    dev_.pollConfig();
    dev_.snapshotConfig(cfg_, cfgVersion_);
    if (cfg_.fpsLimit != 0.f) limitFrameRate();  // before mutex_: never block acquire while sleeping
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
    acquireCv_.notify_all();

    if (surfaceChanged_.exchange(false)) followSurface();
    if (cfg_.gpuBudget > 0.f && generating_) applyGpuBudget();

    // The previous job must have been fully submitted by the worker before
    // we overwrite the images it copies from; this is also where FIFO
    // back-pressure reaches the application.
    VkResult pending;
    {
        std::unique_lock<std::mutex> wlock(workerMutex_);
        // Low-latency: do not let the game run ahead of the presentation
        // engine (with FIFO, queued frames are pure display latency).
        if (cfg_.lowLatency) waitWorkerIdle(wlock);
        else waitWorkerSubmitted(wlock);
        pending = pendingResult_;
        if (pending != VK_ERROR_OUT_OF_DATE_KHR) pendingResult_ = VK_SUCCESS;
    }
    if (pending == VK_ERROR_OUT_OF_DATE_KHR) {
        consumeSemaphores(appWaits, appWaitCount);
        return pending;
    }

    // Frame interval estimate (EMA) for pacing.
    double dt = 0.0;
    if (lastAppPresent_.time_since_epoch().count() != 0) {
        dt = std::chrono::duration<double, std::milli>(t0 - lastAppPresent_).count();
        if (dt > 0.5 && dt < 500.0) frameIntervalMs_ = frameIntervalMs_ * 0.8 + dt * 0.2;
    }
    lastAppPresent_ = t0;

    const int mult = generating_ ? chooseMultiplier() : 1;
    // A rebuilt output stage has no analysis of the previous frame yet.
    const bool haveHistory = prevIndex_ >= 0 && !historyLost_;
    const bool interpolate = generating_ && haveHistory && mult > 1;
    historyLost_ = false;
    const int prev = prevIndex_;
    if (t0 - statusAt_ >= std::chrono::seconds(1)) {  // the control panel's live view
        statusAt_ = t0;
        publishStatus(mult);
    }
    if (cfg_.overlayMode > 0) {
        if (dt > 0.5 && dt < 500.0) updateHudLows(dt);
        const int gfps = frameIntervalMs_ > 0.05 ? static_cast<int>(1000.0 / frameIntervalMs_ + 0.5) : 0;
        const double latency = displayLatencyMs();
        fg_->setHud(gfps, gfps * (interpolate ? mult : 1), hudLow1_, hudLow01_,
                    latency < 0.0 ? -1 : static_cast<int>(latency + 0.5));
    }

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
        FrameGen::FrameTiming timing;
        timing.sinceLastMs = dt;
        timing.intervalMs = frameIntervalMs_;
        timing.multiplier = interpolate ? mult : 1;
        timing.reset = !haveHistory || !(dt > 0.5 && dt < 500.0);
        // Measured refresh grid: predict where each of this present's frames
        // lands (the pacing presents frame k for the refresh after
        // start + I*k/m; frame 0 goes out as soon as the GPU work is done,
        // about now), so frame_timing places the content on real display times.
        double lastMs, periodMs;
        if (interpolate && feedback_ && displayGrid(lastMs, periodMs)) {
            const double now = msOf(t0);
            auto land = [&](double ms) { return lastMs + std::ceil((ms - lastMs) / periodMs) * periodMs; };
            double at[4] = {};
            for (int k = 0; k < mult && k < 4; ++k)
                at[k] = (k == 0 ? land(now + 1.0) : land(now + frameIntervalMs_ * k / mult)) - now;
            timing.haveDisplay = true;
            if (cfg_.extrapolate) {  // real first, then outputs 1..m-1
                timing.displayMs[3] = float(at[0]);
                for (int i = 1; i < mult; ++i) timing.displayMs[i - 1] = float(at[i]);
            } else {                        // outputs first, the real frame last
                for (int i = 1; i < mult; ++i) timing.displayMs[i - 1] = float(at[i - 1]);
                timing.displayMs[3] = float(at[mult - 1]);
            }
        }
        fg_->recordFrameState(slot.cmd, timing);
        fg_->profileMark(slot.cmd, slotIdx, FrameGen::StageAnalysis);
        if (interpolate) {
            fg_->recordFlow(slot.cmd, parity_);
            fg_->profileMark(slot.cmd, slotIdx, FrameGen::StageFlow);
            // Interpolation: frames between prev and cur (t in (0,1)).
            // Extrapolation: frames after cur (t in (1,2)), shown after it.
            const float base = cfg_.extrapolate ? 1.f : 0.f;
            for (int i = 1; i < mult; ++i)
                fg_->recordInterpolate(slot.cmd, static_cast<uint32_t>(prev), imageIndex, base + float(i) / mult, static_cast<uint32_t>(i - 1));
            fg_->profileMark(slot.cmd, slotIdx, FrameGen::StageInterp);
        }
        // Resample the real frame into the internal image the worker presents
        // (for upscaling, or so the HUD can be drawn on it).
        if (fg_->packReal()) fg_->recordUpscaleReal(slot.cmd, imageIndex, interpolate ? prev : -1);
        if (dumping_) {
            if (interpolate)
                for (int i = 0; i < mult - 1; ++i) fg_->recordDump(slot.cmd, static_cast<uint32_t>(i), imageIndex);
            fg_->recordDump(slot.cmd, fg_->outputCount(), imageIndex);
        }
        fg_->recordReleaseSources(slot.cmd, prev, static_cast<int>(imageIndex));
    } else if (fg_->packReal()) {
        // No frame generation: still process the one real frame (upscale / HUD).
        fg_->recordAcquireSources(slot.cmd, -1, static_cast<int>(imageIndex));
        fg_->recordUpscaleReal(slot.cmd, imageIndex);
        if (dumping_) fg_->recordDump(slot.cmd, fg_->outputCount(), imageIndex);
        fg_->recordReleaseSources(slot.cmd, -1, static_cast<int>(imageIndex));
    } else if (dumping_) {
        // Ground-truth runs use multiplier=1: copy the unmodified source into
        // the dump buffer while the image is temporarily in GENERAL layout.
        fg_->recordAcquireSources(slot.cmd, -1, static_cast<int>(imageIndex));
        fg_->recordDump(slot.cmd, fg_->outputCount(), imageIndex);
        fg_->recordReleaseSources(slot.cmd, -1, static_cast<int>(imageIndex));
    }
    // Pass-through: an empty submission still consumes the application's
    // semaphores and orders the worker's copy after its rendering.
    dev_.vt.EndCommandBuffer(slot.cmd);

    // Extrapolation presents the real frame first. When it is copied straight
    // from the application's image (no upscaling / HUD / repeat replacement,
    // which the synthesis produces), the synthesis is handed to the worker
    // and submitted after that copy: the real frame then leaves as soon as
    // the game's rendering is done instead of queueing behind the analysis,
    // flow and synthesis (their GPU time is the latency saved). Here only the
    // game's semaphores are consumed, which orders the worker's copy after
    // its rendering. Not while dumping (dumpFrame waits on the slot at once).
    const bool deferSynthesis = cfg_.extrapolate && interpolate && !fg_->packReal() && !dumping_;
    std::vector<VkPipelineStageFlags> stages(appWaitCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = appWaitCount;
    si.pWaitSemaphores = appWaits;
    si.pWaitDstStageMask = stages.data();
    si.commandBufferCount = deferSynthesis ? 0 : 1;
    si.pCommandBuffers = &slot.cmd;
    VkResult r;
    std::vector<VkSemaphore> copyWaits;
    if (dev_.asyncCompute) {
        // Two queues: nothing is ordered by queue order any more. The copies
        // of the previous jobs must be done before this synthesis rewrites
        // what they read, and each of this job's presents waits the
        // synthesis (or, deferred, the game) through its own semaphore.
        {
            std::lock_guard<std::mutex> wlock(workerMutex_);
            copyWaits.swap(pendingCopyDone_);
        }
        const int frames = interpolate ? mult : 1;
        if (deferSynthesis) {
            VkSemaphore outs[2] = {slot.realIn, slot.synthIn};
            si.signalSemaphoreCount = 2;
            si.pSignalSemaphores = outs;
            r = submitOn(dev_.queue, si, VK_NULL_HANDLE);
        } else {
            std::vector<VkSemaphore> waits(appWaits, appWaits + appWaitCount);
            waits.insert(waits.end(), copyWaits.begin(), copyWaits.end());
            std::vector<VkPipelineStageFlags> waitStages(waits.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            si.waitSemaphoreCount = static_cast<uint32_t>(waits.size());
            si.pWaitSemaphores = waits.data();
            si.pWaitDstStageMask = waitStages.data();
            si.signalSemaphoreCount = static_cast<uint32_t>(frames);
            si.pSignalSemaphores = slot.synthDone;
            r = submitOn(dev_.computeQueue, si, slot.fence);
            copyWaits.clear();
        }
    } else {
        r = dev_.submit(si, deferSynthesis ? VK_NULL_HANDLE : slot.fence);
    }
    if (r < 0) {
        BDEX_ERR("synthesis submission failed: %s", vkResultName(r));
        consumeSemaphores(appWaits, appWaitCount);
        return r;
    }
    // Deferred: the worker submits this slot before it finishes the job, and
    // every wait on its fence happens after that (nextSlot comes after
    // waitWorkerSubmitted; acquire only polls; waitIdle waits for the worker).
    slot.submitted = true;
    // The previous frame was read for the last time by this submission; the
    // current one stays held until the next present reads it as `prev`.
    if (prev >= 0) {
        VirtualImage& pv = virtualImages_[prev];
        pv.held = false;
        pv.pendingRelease = true;
        pv.releaseSlot = slotIdx;
        pv.releaseGeneration = slot.generation;
        acquireCv_.notify_all();
    }
    vi.held = true;
    if (dumping_) {
        // Written in presentation order.
        if (cfg_.extrapolate) {
            dumpFrame(slot, false, 1.f);
            if (interpolate)
                for (int i = 1; i < mult; ++i)
                    dumpFrame(slot, true, 1.f + float(i) / mult, static_cast<uint32_t>(i - 1));
        } else {
            if (interpolate)
                for (int i = 1; i < mult; ++i)
                    dumpFrame(slot, true, float(i) / mult, static_cast<uint32_t>(i - 1));
            dumpFrame(slot, false, 1.f);
        }
    }

    // Hand the presentation over to the worker.
    Job job;
    job.parity = parity_;
    job.source = imageIndex;
    job.frames = interpolate ? mult : 1;
    job.generated = interpolate;
    job.realFirst = cfg_.extrapolate;
    job.start = t0;
    job.intervalMs = frameIntervalMs_;
    job.pacing = cfg_.pacing;
    if (deferSynthesis) job.synthSlot = static_cast<int>(slotIdx);
    if (dev_.asyncCompute) {
        job.asyncSlot = static_cast<int>(slotIdx);
        job.synthWaits = std::move(copyWaits);  // deferred: the synthesis the worker submits waits them
    }
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

    // Synchronous presentation: block the game's present call until the worker
    // has actually presented this frame. Some Direct3D-12 translation layers
    // (VKD3D-Proton) tie their frame pacing to a synchronous present and hang
    // otherwise. This serialises the worker with the game, so it is a
    // compatibility fallback, not the default.
    if (cfg_.syncPresent) {
        std::unique_lock<std::mutex> wlock(workerMutex_);
        waitWorkerIdle(wlock);
    }

    prevIndex_ = static_cast<int>(imageIndex);
    parity_ ^= 1;

    if (cfg_.stats) {
        statsAppFrames_ += 1;
        statsCpuMs_ += std::chrono::duration<double, std::milli>(clock::now() - t0).count();
        const auto now = clock::now();
        const double elapsed = std::chrono::duration<double>(now - statsStart_).count();
        if (elapsed >= cfg_.statsInterval) {
            const uint64_t out = statsOutFrames_.exchange(0);
            const uint64_t delayUs = statsRealDelayUs_.exchange(0);
            const uint64_t skipped = statsSkipped_.exchange(0);
            char skip[256] = "";
            int len = 0;
            if (skipped) len = snprintf(skip, sizeof(skip), ", %llu predicted frame(s) skipped", static_cast<unsigned long long>(skipped));
            if (feedback_) {
                std::lock_guard<std::mutex> lock(fbMutex_);
                const double hz = displayClock_.refreshHz();
                char buf[96];
                const int k = hz > 0.0 ? snprintf(buf, sizeof buf, ", display %.1f Hz", hz)
                                       : snprintf(buf, sizeof buf, ", display without fixed refresh");
                if (k > 0 && len + k < int(sizeof(skip))) { memcpy(skip + len, buf, size_t(k) + 1); len += k; }
                if (fbLatencyCount_) {
                    char lat[64];
                    const int m = snprintf(lat, sizeof lat, ", present-to-screen %.1f ms", fbLatencySum_ / fbLatencyCount_);
                    if (m > 0 && len + m < int(sizeof(skip))) { memcpy(skip + len, lat, size_t(m) + 1); len += m; }
                    fbLatencySum_ = 0.0;
                    fbLatencyCount_ = 0;
                }
                const uint64_t replaced = fbReplaced_ - statsReplaced_;
                statsReplaced_ = fbReplaced_;
                if (replaced) len += snprintf(skip + len, sizeof(skip) - size_t(len), ", %llu frame(s) replaced before display",
                                             static_cast<unsigned long long>(replaced));
            }
            // dedup: read without waiting (the counter is at most a few frames old).
            FrameGen::State st;
            if (cfg_.dedup && fg_->readState(st)) {
                snprintf(skip + len, sizeof(skip) - size_t(len), ", %u repeated frame(s)", st.repeats - statsRepeats_);
                statsRepeats_ = st.repeats;
            }
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
    // async_compute: every semaphore a present would have waited must be
    // waited even when that present is skipped or fails before its copy is
    // submitted, or it stays signalled into its next use.
    std::vector<bool> copied(static_cast<size_t>(job.frames), false);
    auto consumeUnused = [&] {
        if (job.asyncSlot < 0) return;
        const FrameSlot& synth = slots_[static_cast<size_t>(job.asyncSlot)];
        std::vector<VkSemaphore> left;
        for (int k = 0; k < job.frames; ++k) {
            if (copied[size_t(k)]) continue;
            const bool realK = job.realFirst ? k == 0 : k == job.frames - 1;
            left.push_back((job.synthSlot >= 0 && realK) ? synth.realIn : synth.synthDone[k]);
        }
        if (!left.empty()) consumeSemaphores(left.data(), static_cast<uint32_t>(left.size()));
    };
    for (int i = 0; i < job.frames; ++i) {
        lastCopySubmitted_ = false;
        VkResult r = presentOne(job, i, i == job.frames - 1 ? &markSubmitted : nullptr);
        copied[size_t(i)] = lastCopySubmitted_;
        // After the real frame (index 0 of a deferred job), whatever its
        // outcome: the slot's fence must signal for the application thread.
        if (i == 0 && job.synthSlot >= 0) {
            const VkResult sr = submitDeferredSynthesis(job);
            if (r >= 0 && sr < 0) r = sr;
        }
        if (r == VK_EVENT_RESET) {  // predicted frame skipped: a newer real frame is waiting
            statsSkipped_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        if (r < 0) {
            consumeUnused();
            markSubmitted();
            // A genuine out-of-date on the real swapchain means the window
            // changed (e.g. a resize). On Wayland, drop the reduced-extent flag
            // so the app's recreation re-learns the new true window size before
            // we reduce again (see WaylandSurfaceState).
            if (r == VK_ERROR_OUT_OF_DATE_KHR && surface_ != VK_NULL_HANDLE) {
                std::lock_guard<std::mutex> lk(dev_.inst->surfMutex);
                auto it = dev_.inst->waylandSurfaces.find(surface_);
                if (it != dev_.inst->waylandSurfaces.end()) it->second.reduce = false;
            }
            std::lock_guard<std::mutex> wlock(workerMutex_);
            pendingResult_ = r;
            acquireCv_.notify_all();
            return;
        }
        // VK_SUBOPTIMAL is deliberately not forwarded: it reports that *our*
        // real swapchain is no longer optimal, but the application's virtual
        // swapchain is unaffected, so we must not push it into recreating (some
        // D3D translation layers hang while recreating). Instead the
        // application thread re-creates the real swapchain if the surface
        // changed size (followSurface). A genuine change fails the
        // acquire/present with OUT_OF_DATE (r < 0 above), which we do
        // propagate so the application recreates and we rebuild the real one.
        if (r == VK_SUBOPTIMAL_KHR) surfaceChanged_.store(true, std::memory_order_relaxed);
        statsOutFrames_.fetch_add(1, std::memory_order_relaxed);
    }
    consumeUnused();
    markSubmitted();
}

// ---------------------------------------------------------------------------
// Present feedback
// ---------------------------------------------------------------------------

// Waits for each presented id in turn. vkWaitForPresentKHR returns once that
// frame *or a later one* is displayed, so an id that completes right after
// its predecessor (well within a refresh) means the predecessor was replaced
// before being shown (mailbox), and the display time belongs to this one.
void VirtualSwapchain::feedbackLoop() {
    for (;;) {
        FbEntry entry;
        {
            std::unique_lock<std::mutex> lock(fbMutex_);
            fbCv_.wait(lock, [this] { return fbStop_ || (!fbPaused_ && !fbPending_.empty()); });
            if (fbStop_) return;
            entry = fbPending_.front();
            fbPending_.pop_front();
            fbBusy_ = true;
        }
        VkResult r = VK_TIMEOUT;
        // Short waits so a pause (swapchain re-creation) is honoured quickly;
        // a frame not shown within ~0.5 s (minimised window) is given up.
        for (int k = 0; k < 25 && r == VK_TIMEOUT && !fbPaused_; ++k)
            r = dev_.vt.WaitForPresentKHR(dev_.device, real_, entry.id, 20'000'000ull);
        const double now = msOf(clock::now());
        bool quit = false;
        {
            std::lock_guard<std::mutex> lock(fbMutex_);
            fbBusy_ = false;
            if (r >= 0 && r != VK_TIMEOUT) {
                fbEverDisplayed_ = true;
                // Latency the player sees: the game's present call to the
                // moment its real frame is on screen (all queueing, the
                // layer's hold and the compositor included).
                const double latency = now - entry.presentMs;
                if (entry.real && latency >= 0.0 && latency < 1000.0) {
                    fbLatencyMs_ = fbLatencyMs_ < 0.0 ? latency : fbLatencyMs_ * 0.9 + latency * 0.1;
                    fbLatencySum_ += latency;
                    ++fbLatencyCount_;
                }
                if (fbLastMs_ > 0.0 && now - fbLastMs_ < 0.5) {
                    ++fbReplaced_;
                } else {
                    displayClock_.add(now);
                    fbLastMs_ = now;
                }
            } else if (r == VK_TIMEOUT && !fbPaused_ && !fbEverDisplayed_ && ++fbTimeouts_ >= 30) {
                // Nothing ever reported displayed: the driver or compositor
                // does not implement present waits usefully here.
                BDEX_INFO("present feedback unavailable on this surface; disabled");
                feedback_ = false;
                quit = true;
            }
        }
        fbIdleCv_.notify_all();
        if (quit) return;
    }
}

void VirtualSwapchain::pauseFeedback() {
    if (!fbThread_.joinable()) return;
    std::unique_lock<std::mutex> lock(fbMutex_);
    fbPaused_ = true;
    fbPending_.clear();
    fbIdleCv_.wait(lock, [this] { return !fbBusy_; });
}

void VirtualSwapchain::resumeFeedback() {
    if (!fbThread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(fbMutex_);
        fbPaused_ = false;
        fbLastMs_ = 0.0;
        presentCounter_ = 0;  // ids restart with the new swapchain (the worker is idle here)
    }
    fbCv_.notify_all();
}

double VirtualSwapchain::displayLatencyMs() {
    if (!feedback_) return -1.0;
    std::lock_guard<std::mutex> lock(fbMutex_);
    return fbLatencyMs_;
}

double VirtualSwapchain::displayHz() {
    std::lock_guard<std::mutex> lock(fbMutex_);
    return displayClock_.refreshHz();
}

bool VirtualSwapchain::displayGrid(double& lastMs, double& periodMs) {
    std::lock_guard<std::mutex> lock(fbMutex_);
    if (!displayClock_.valid()) return false;
    lastMs = displayClock_.lastMs();
    periodMs = displayClock_.periodMs();
    return true;
}

// Submits the synthesis the application thread recorded but left to the
// worker (Job::synthSlot), right after the real frame went out.
VkResult VirtualSwapchain::submitDeferredSynthesis(const Job& job) {
    FrameSlot& s = slots_[static_cast<size_t>(job.synthSlot)];
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.cmd;
    // async_compute: after the game (synthIn) and the copies still reading
    // the outputs; signals the generated frames' presents (synthDone[1..]).
    std::vector<VkSemaphore> waits;
    std::vector<VkPipelineStageFlags> stages;
    VkQueue queue = dev_.queue;
    if (job.asyncSlot >= 0) {
        queue = dev_.computeQueue;
        waits.push_back(s.synthIn);
        waits.insert(waits.end(), job.synthWaits.begin(), job.synthWaits.end());
        {
            std::lock_guard<std::mutex> wlock(workerMutex_);  // this job's real copy
            waits.insert(waits.end(), pendingCopyDone_.begin(), pendingCopyDone_.end());
            pendingCopyDone_.clear();
        }
        stages.assign(waits.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        si.waitSemaphoreCount = static_cast<uint32_t>(waits.size());
        si.pWaitSemaphores = waits.data();
        si.pWaitDstStageMask = stages.data();
        si.signalSemaphoreCount = static_cast<uint32_t>(job.frames - 1);
        si.pSignalSemaphores = s.synthDone + 1;
    }
    std::lock_guard<std::mutex> qlock(dev_.queueMutex(queue));
    VkResult r = dev_.vt.QueueSubmit(queue, 1, &si, s.fence);
    if (r < 0) {
        BDEX_ERR("synthesis submission failed: %s", vkResultName(r));
        // Still signal the fence (and the semaphores the presents wait) so
        // nothing waits forever; the waits above are consumed too.
        VkSubmitInfo empty = si;
        empty.commandBufferCount = 0;
        dev_.vt.QueueSubmit(queue, 1, &empty, s.fence);
    }
    if (job.presentFence) {
        VkSubmitInfo fs{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        dev_.vt.QueueSubmit(queue, 1, &fs, job.presentFence);
    }
    return r;
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
    if (index > 0 && job.pacing && presentMode_ != VK_PRESENT_MODE_FIFO_KHR &&
        presentMode_ != VK_PRESENT_MODE_FIFO_RELAXED_KHR) {
        auto target = job.start + std::chrono::duration_cast<clock::duration>(
                                      std::chrono::duration<double, std::milli>(job.intervalMs * index / job.frames));
        // With a known refresh grid (present feedback, fixed refresh), aim
        // at the refresh this frame should show on and present well before
        // it: a present landing just after a refresh would slip a whole
        // period (judder). Unknown grid (VRR, no feedback): as scheduled.
        double lastMs, periodMs;
        if (feedback_ && displayGrid(lastMs, periodMs)) {
            const double land = lastMs + std::ceil((msOf(target) - lastMs) / periodMs) * periodMs;
            target = clock::time_point(std::chrono::duration_cast<clock::duration>(
                std::chrono::duration<double, std::milli>(land - 0.3 * periodMs)));
        }
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
    // (async_compute: two queues, so the synthesis — or, for a deferred
    // job's real frame, the game — is waited through a semaphore, and the
    // copy signals when it is done reading for the next synthesis.)
    VkPipelineStageFlags stages[2] = {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
    VkSemaphore waits[2] = {slot.acquireSem, VK_NULL_HANDLE};
    VkSemaphore signals[2] = {realImages_[ri].readySem, slot.copyDone};
    const bool async = job.asyncSlot >= 0;
    if (async) {
        const FrameSlot& synth = slots_[static_cast<size_t>(job.asyncSlot)];
        waits[1] = (job.synthSlot >= 0 && real) ? synth.realIn : synth.synthDone[index];
    }
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = async ? 2 : 1;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = stages;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &slot.cmd;
    si.signalSemaphoreCount = async ? 2 : 1;
    si.pSignalSemaphores = signals;

    VkPresentIdKHR presentId{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
    VkSwapchainPresentModeInfoEXT presentMode{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT};
    const VkPresentModeKHR presentModeValue = static_cast<VkPresentModeKHR>(job.presentMode);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &realImages_[ri].readySem;
    pi.swapchainCount = 1;
    pi.pSwapchains = &real_;
    pi.pImageIndices = &ri;
    const void** tail = &pi.pNext;
    uint64_t feedbackId = 0;
    if (real && job.presentId) {
        presentId.swapchainCount = 1;
        presentId.pPresentIds = &job.presentId;
        *tail = &presentId;
        tail = &presentId.pNext;
    } else if (feedback_ && !job.presentId) {
        // Our own ids (the game cannot use present ids: they are hidden).
        feedbackId = ++presentCounter_;
        presentId.swapchainCount = 1;
        presentId.pPresentIds = &feedbackId;
        *tail = &presentId;
        tail = &presentId.pNext;
    }
    // The application's present fence (VK_EXT_swapchain_maintenance1) signals
    // when its swapchain image is free to reuse. Its image is our *virtual*
    // image, freed once our copy has read it — not when the real frame finishes
    // displaying. So we signal that fence ourselves after the copy (below),
    // instead of tying it to the real present; otherwise the game blocks
    // forever waiting on a display completion that our async worker owns.
    if (job.presentMode >= 0) {
        presentMode.swapchainCount = 1;
        presentMode.pPresentModes = &presentModeValue;
        *tail = &presentMode;
        tail = &presentMode.pNext;
    }

    std::lock_guard<std::mutex> qlock(dev_.queueMutex(dev_.queue));
    r = dev_.vt.QueueSubmit(dev_.queue, 1, &si, slot.fence);
    if (r < 0) return r;
    lastCopySubmitted_ = true;
    if (async) {
        std::lock_guard<std::mutex> wlock(workerMutex_);
        pendingCopyDone_.push_back(slot.copyDone);
    }
    // Signal the application's present fence after the copy (queue-ordered): its
    // virtual image is then free to reuse, so its next frame can proceed. A
    // deferred synthesis still reads it: submitDeferredSynthesis signals then.
    if (real && job.presentFence && job.synthSlot < 0) {
        VkSubmitInfo fs{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        dev_.vt.QueueSubmit(dev_.queue, 1, &fs, job.presentFence);
    }
    slot.submitted = true;
    if (onSubmitted) (*onSubmitted)();
    if (real)
        statsRealDelayUs_.fetch_add(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - job.start).count()),
            std::memory_order_relaxed);
    r = dev_.vt.QueuePresentKHR(dev_.queue, &pi);
    if (feedbackId && r >= 0) {
        {
            std::lock_guard<std::mutex> lock(fbMutex_);
            if (!fbPaused_) fbPending_.push_back({feedbackId, msOf(job.start), real});
        }
        fbCv_.notify_one();
    }
    if (r == VK_SUCCESS && suboptimal) r = VK_SUBOPTIMAL_KHR;
    return r;
}

} // namespace bdex
