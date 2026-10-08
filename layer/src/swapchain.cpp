#include "swapchain.h"
#include "frame_order.h"
#include "gamescope.h"
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
constexpr uint32_t kWorkerSlots = 16;  // (FrameGen::kProfileSlots = kAppSlots + kWorkerSlots)

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
        prepareStaging();
        outputGen_ = cfg_.rebuildGen;
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
    if (cfg_.requireApp && !cfg_.appTargeted) e.state = "ready";  // armed, waiting for « Démarrer »
    if (!generating_) e.reason = genFailReason_.empty() ? "frame generation off for this swapchain" : genFailReason_;
    e.mode = cfg_.modeName();
    e.multiplier = on ? multiplier : 1;
    e.gameFps = frameIntervalMs_ > 0.05 ? 1000.0 / frameIntervalMs_ : 0.0;
    e.outFps = e.gameFps * e.multiplier;
    e.refreshHz = displayHz();
    e.limitHz = gsLimited_.load() ? gsHz_.load() : 0.0;
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
        // (require_app: the control panel may raise it to x4 while the game runs.)
        // The worker also acquires a present's frames ahead of presenting them
        // (runJob): up to `mult` at once, which the surface allows only with
        // imageCount - minImageCount >= mult - 1 (VUID-vkAcquireNextImageKHR-
        // swapchain-01802: more, and the driver may hand back an image still
        // being presented).
        const uint32_t mult = static_cast<uint32_t>(cfg_.requireApp ? std::max(4, cfg_.multiplier) : std::max(1, cfg_.multiplier));
        uint32_t want = std::max({appInfo.minImageCount, 2u + mult, caps.minImageCount + mult - 1});
        if (caps.maxImageCount) want = std::min(want, caps.maxImageCount);
        ci.minImageCount = std::max(want, caps.minImageCount);
        surfaceMinImages_ = caps.minImageCount;
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            throw VkError(VK_ERROR_FEATURE_NOT_PRESENT, "surface does not support transfer destination images");
    }

    VkBool32 supported = VK_FALSE;
    dev_.inst->vt.GetPhysicalDeviceSurfaceSupportKHR(dev_.physDev, dev_.queueFamily, appInfo.surface, &supported);
    if (!supported) throw VkError(VK_ERROR_FEATURE_NOT_PRESENT, "layer queue family cannot present to this surface");

    VkResult r = dev_.vt.CreateSwapchainKHR(dev_.device, &ci, nullptr, &real_);
    if (r < 0) throw VkError(r, "vkCreateSwapchainKHR (real)");
    realNow_ = real_;
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
    // Images the worker may hold acquired at once without breaking the
    // surface's forward-progress guarantee (see createReal).
    acquireAhead_ = std::max(1, static_cast<int>(n) - static_cast<int>(surfaceMinImages_) + 1);
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
    surfaceMinImages_ = caps.minImageCount;
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
    realNow_ = real_;
    fg_ = std::move(fg);
    fg_->setQualityLevel(budget_.level());
    generating_ = generating;
    historyLost_ = true;
    try {
        createRealImages();
        prepareStaging();
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
        if (!withSemaphore) VK_CHECK(dev_.vt.CreateFence(dev_.device, &fi, nullptr, &s.readyFence));
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
        if (s.readySubmitted) dev_.vt.WaitForFences(dev_.device, 1, &s.readyFence, VK_TRUE, UINT64_MAX);
        if (s.cmd && pool) dev_.vt.FreeCommandBuffers(dev_.device, pool, 1, &s.cmd);
        if (s.fence) dev_.vt.DestroyFence(dev_.device, s.fence, nullptr);
        if (s.readyFence) dev_.vt.DestroyFence(dev_.device, s.readyFence, nullptr);
        for (VkSemaphore sem : {s.acquireSem, s.realIn, s.synthIn, s.copyDone})
            if (sem) dev_.vt.DestroySemaphore(dev_.device, sem, nullptr);
        for (VkSemaphore sem : s.synthDone)
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
    realNow_ = VK_NULL_HANDLE;
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
        // On the synthesis queue: a signal waits for every earlier submission
        // of its queue, so the game's next frame starts on the GPU after the
        // synthesis already queued — which the frame being paced needs first
        // (the image itself is free: its last reader's fence was checked).
        VkResult r = submitOn(dev_.asyncCompute ? dev_.computeQueue : dev_.queue, si, fence);
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
    if (s.readySubmitted) {  // (signalled before the synthesis the fence above waited)
        dev_.vt.WaitForFences(dev_.device, 1, &s.readyFence, VK_TRUE, UINT64_MAX);
        dev_.vt.ResetFences(dev_.device, 1, &s.readyFence);
        s.readySubmitted = false;
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
// is set: then the largest multiplier whose output stays at or under the
// target — the refresh rate — (on 144 Hz: 72 fps gives x2, 50 fps x2, 45 fps
// x3, 36 fps x4, x1 from 74 fps), capped at `multiplier`, which the output
// stage was sized for (dynamicMultiplier, display_clock.h).
int VirtualSwapchain::chooseMultiplier() {
    const int maxMult = std::max(1, cfg_.multiplier);
    if (cfg_.paused) return 1;
    // Steam's framerate limit (gamescope): the screen shows at most one frame
    // per cycle of the limit, and the frames past it hold the game back — so
    // generate only up to the limit, whatever the multiplier: adaptive with
    // the limit as target (a game already at the limit gets no generated
    // frame, one under it gets the missing ones).
    const bool limited = gsLimited_.load() && gsHz_.load() > 0.0;
    // auto: the refresh rate (measured, else the control panel's); unknown:
    // x2, the output a display is the least likely to overflow (auto usually
    // comes with multiplier = 4, the ceiling, not a choice).
    // (adaptive without a target: the refresh rate, like auto)
    const bool autoTarget = limited || cfg_.targetFps < 0.f || (cfg_.adaptive && cfg_.targetFps == 0.f);
    const double target = limited ? gsHz_.load() : autoTarget ? displayHz() : double(cfg_.targetFps);
    adaptivePeriodMs_ = 0.0;
    if (target <= 0.0) return autoTarget ? std::min(2, maxMult) : maxMult;
    if (cfg_.adaptive || limited) {
        // As many frames as fit at the target rate, the fraction carried over
        // (50 fps on 144 Hz: 3, 3, 2, 3, 3, 2...), spaced one output period
        // apart (the display's refresh when target_fps is auto): every
        // refresh shows a new frame — the whole refresh rate, not the largest
        // whole multiple under it (x2 = 100 fps there). Past the ceiling
        // (the game too slow) or under one frame (faster than the target),
        // nothing is carried.
        adaptivePeriodMs_ = 1000.0 / target;
        // (Under the limit, when it waits here a quarter of its frame or
        // more, the game's own frame time: those waits for the display would
        // make it look slower, ask for more frames and slow it further — 30
        // fps settled at 23, x1.3. Short waits stay counted: without them the
        // output fell short of the limit, 56 fps for 60.)
        const bool held = limited && gameOwnMs_ > 0.0 && frameIntervalMs_ - gameOwnMs_ > 0.25 * frameIntervalMs_;
        const double interval = held ? gameOwnMs_ : frameIntervalMs_;
        adaptiveAcc_ += interval / adaptivePeriodMs_;
        const int n = std::clamp(static_cast<int>(adaptiveAcc_), 1, maxMult);
        adaptiveAcc_ = std::clamp(adaptiveAcc_ - n, 0.0, 0.999);
        if (n < 2) adaptivePeriodMs_ = 0.0;
        return n;
    }
    dynMult_ = dynamicMultiplier(dynMult_, target * frameIntervalMs_ / 1000.0, maxMult);
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
            prepareStaging();
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

// require_app: the control panel changed a setting that sizes the output
// stage (multiplier, mode, flow, HUD...) or started / stopped the generation
// here: rebuild it like applyGpuBudget does (one frame without generation).
// Called under mutex_ from present().
void VirtualSwapchain::rebuildOutput() {
    outputGen_ = cfg_.rebuildGen;
    {
        std::unique_lock<std::mutex> wlock(workerMutex_);
        waitWorkerIdle(wlock);
    }
    waitQueuesIdle();
    bool generating = cfg_.enabled && cfg_.debug != Config::Debug::Passthrough && cfg_.multiplier > 1 && !throwaway_;
    try {
        auto fg = makeFrameGen(generating);
        fg_ = std::move(fg);
        fg_->setQualityLevel(budget_.level());
        generating_ = generating;
        prepareStaging();
        historyLost_ = true;
        dynMult_ = 0;
    } catch (const VkError& e) {
        BDEX_WARN("control panel: output stage not rebuilt (%s)", e.what());
        return;
    }
    BDEX_INFO("control panel: output stage rebuilt (x%d %s, generation %s)", cfg_.multiplier, cfg_.modeName(),
              generating_ && !cfg_.paused ? "on" : "paused");
}

VkResult VirtualSwapchain::present(uint32_t imageIndex, const VkPresentInfoKHR& info, bool consumeWaitSemaphores) {
    dev_.pollConfig();
    dev_.snapshotConfig(cfg_, cfgVersion_);
    if (cfg_.fpsLimit != 0.f) limitFrameRate();  // before mutex_: never block acquire while sleeping
    std::unique_lock<std::mutex> lock(mutex_);
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

    // The refresh rate the system reports: gamescope's cycle, else the
    // control panel's reading (require_app).
    if (const double hint = gsHz_.load() > 0.0 ? gsHz_.load() : cfg_.panelDisplayHz; hint != clockHintHz_) {
        clockHintHz_ = hint;
        std::lock_guard<std::mutex> fbLock(fbMutex_);
        displayClock_.setHint(clockHintHz_);
    }
    if (const bool limited = gsLimited_.load(); limited != gsLimitLogged_) {
        gsLimitLogged_ = limited;
        if (limited)
            BDEX_INFO("gamescope frame limit on (Steam's framerate limit, %.0f fps): the screen shows no more, "
                      "so frames are generated only up to it; set it to Off or to the refresh rate to generate above",
                      gsHz_.load());
        else
            BDEX_INFO("gamescope frame limit off: display %.1f Hz", gsHz_.load());
    }
    if (surfaceChanged_.exchange(false)) followSurface();
    if (cfg_.rebuildGen != outputGen_) rebuildOutput();
    if (cfg_.gpuBudget > 0.f && generating_) applyGpuBudget();

    // The previous job must have been fully submitted by the worker before
    // we overwrite the images it copies from; this is also where FIFO
    // back-pressure reaches the application.
    VkResult pending;
    double workerWaitMs = 0.0;
    const bool limitedByGamescope = gsLimited_.load() && feedback_.load();
    const bool gateOnDisplay = limitedByGamescope && lastPresentFrames_ <= 1;
    {
        std::unique_lock<std::mutex> wlock(workerMutex_);
        // Low-latency: do not let the game run ahead of the presentation
        // engine (with FIFO, queued frames are pure display latency).
        // (Predicted frames still waiting are not dropped here: this frame is
        // only submitted, its rendering may end a frame from now. The worker
        // drops them once it is ready to be shown — Job::ready.)
        // Under Steam's framerate limit the worker hands gamescope a frame
        // once the previous one is on screen (presentCopied). A game at the
        // limit, without generated frames, also waits for its previous frame
        // to be shown, like DXVK there: it begins the next with no frame
        // queued — running at the limit, it would never drain a queue. (Not
        // while generating: a game under the limit would wait for all its
        // frames and fall to half the limit, at x2.)
        const auto w0 = clock::now();
        if (cfg_.lowLatency || gateOnDisplay) waitWorkerIdle(wlock);
        else waitWorkerSubmitted(wlock);
        if (gateOnDisplay && presentCounter_ > 0) {  // (the worker is idle: its counter is still)
            const double hz = gsHz_.load();
            waitDisplayed(presentCounter_, hz > 0.0 ? 3000.0 / hz : 100.0);
        }
        workerWaitMs = std::chrono::duration<double, std::milli>(clock::now() - w0).count();
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
    if (lastReturnMs_ > 0.0) {
        const double own = msOf(t0) - lastReturnMs_;
        if (own > 0.1 && own < 500.0) gameOwnMs_ = gameOwnMs_ > 0.0 ? gameOwnMs_ * 0.8 + own * 0.2 : own;
    }

    // The presentation fell behind: the previous present's frames could not
    // even be submitted — every real image still waits for the display. With
    // generation producing frames at the display's rate, such a queue never
    // drains by itself (measured in gamescope after a 1 s frame limit: 100 ms
    // more from present to screen, for good): this present shows its real
    // frame alone, until the queue is gone.
    const bool lagging = lateJobs_.load(std::memory_order_relaxed) >= 8;
    if (lagging) lateJobs_.store(0, std::memory_order_relaxed);
    const bool catchUp = !cfg_.lowLatency && ((!gateOnDisplay && workerWaitMs > 0.25 * frameIntervalMs_) || lagging);
    if (catchUp) ++statsBehind_;
    const int mult = generating_ && !catchUp ? chooseMultiplier() : 1;
    lastPresentFrames_ = mult;
    // Where the real frame goes among the frames of this present (and, with
    // interp_hold, how far apart; adaptive keeps one output period between
    // all of them).
    FrameOrder order = FrameOrder::of(cfg_, mult, !(adaptivePeriodMs_ > 0.0));
    // Never closer than a refresh apart (also the fastest a VRR screen
    // shows): two frames inside one refresh would replace each other. At
    // 60 fps x2 on 144 Hz that is a hold of 0.83 at least; the measured
    // 0.5 showed 116 of the 120 frames, 0.25 only 95.
    if (order.hold < 1.f && frameIntervalMs_ > 0.0) {
        const double hz = displayHz();
        if (hz > 0.0) order.hold = float(std::min(1.0, std::max<double>(order.hold, 1000.0 / hz * mult / frameIntervalMs_)));
    }
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
        // The output rate the screen really shows when the present feedback
        // tells (frames replaced before being displayed do not count), else
        // the nominal game fps x multiplier.
        int outFps = gfps * (interpolate ? mult : 1);
        if (feedback_) {
            if (t0 - hudRateAt_ >= std::chrono::seconds(1)) {
                uint64_t shown;
                bool ever;
                {
                    std::lock_guard<std::mutex> fbLock(fbMutex_);
                    shown = fbDisplayed_;
                    ever = fbEverDisplayed_;
                }
                const double secs = std::chrono::duration<double>(t0 - hudRateAt_).count();
                if (ever && hudRateAt_.time_since_epoch().count() != 0 && secs < 3.0)
                    hudDisplayedFps_ = static_cast<int>(double(shown - hudDisplayed_) / secs + 0.5);
                hudDisplayed_ = shown;
                hudRateAt_ = t0;
            }
            if (hudDisplayedFps_ >= 0) outFps = hudDisplayedFps_;
        }
        fg_->setHud(gfps, outFps, hudLow1_, hudLow01_, latency < 0.0 ? -1 : static_cast<int>(latency + 0.5));
    }

    // Synthesis: analyse the application's frame (in place, no copy) and
    // generate the intermediate frames into the internal output images.
    FrameSlot& slot = nextSlot(slots_, slotCursor_);
    const uint32_t slotIdx = static_cast<uint32_t>(&slot - slots_.data());
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dev_.vt.BeginCommandBuffer(slot.cmd, &bi);
    fg_->profileBegin(slot.cmd, slotIdx);
    fg_->collectMeanCost(slotIdx);  // the slot's fence was waited in nextSlot
    if (generating_) {
        fg_->recordAcquireSources(slot.cmd, prev, static_cast<int>(imageIndex));
        fg_->recordAnalysis(slot.cmd, parity_, imageIndex);
        FrameGen::FrameTiming timing;
        timing.sinceLastMs = dt;
        timing.intervalMs = frameIntervalMs_;
        timing.multiplier = interpolate ? mult : 1;
        timing.realPos = order.real;
        timing.reset = !haveHistory || !(dt > 0.5 && dt < 500.0);
        // Measured refresh grid: predict where each of this present's frames
        // lands (the pacing presents frame k for the refresh after
        // start + I*k/m; frame 0 goes out as soon as the GPU work is done,
        // about now), so frame_timing places the content on real display times.
        // Adaptive: the frames are one output period apart, not spread over
        // the game's interval — they always need their display times (the
        // fixed i/m would be wrong), predicted on the grid when it is known.
        double lastMs, periodMs;
        const bool adaptive = interpolate && adaptivePeriodMs_ > 0.0;
        const double spacing = adaptive ? adaptivePeriodMs_ : frameIntervalMs_ / mult;
        const bool grid = interpolate && feedback_ && displayGrid(lastMs, periodMs);
        // interp_hold: the outputs read their t from these display times too,
        // so frame_timing follows the uneven spacing.
        const bool held = interpolate && order.hold < 1.f;
        if (grid || adaptive || held) {
            const double now = msOf(t0);
            auto land = [&](double ms) {
                return grid ? lastMs + std::ceil((ms - lastMs) / periodMs) * periodMs : ms;
            };
            double at[8] = {};
            for (int k = 0; k < mult && k < 8; ++k) at[k] = land(now + 1.0 + spacing * k) - now;
            if (!adaptive)  // (historical: frame k at the refresh after start + I*k/m, frame 0 right after the GPU)
                for (int k = 0; k < mult && k < 8; ++k)
                    at[k] = (k == 0 ? land(now + 1.0)
                             : land(now + (held ? frameIntervalMs_ * order.offset(k) : frameIntervalMs_ * k / mult))) - now;
            timing.haveDisplay = true;
            timing.displayMs[7] = float(at[order.real]);
            for (int o = 0; o + 1 < mult && o < 7; ++o) timing.displayMs[o] = float(at[order.position(o)]);
        }
        fg_->recordFrameState(slot.cmd, timing);
        fg_->profileMark(slot.cmd, slotIdx, FrameGen::StageAnalysis);
        if (interpolate) {
            fg_->recordFlow(slot.cmd, parity_, static_cast<int>(slotIdx));
            fg_->profileMark(slot.cmd, slotIdx, FrameGen::StageFlow);
            // Frames between prev and cur (t in (0,1)) before the real frame,
            // frames after cur (t in (1,2)) after it (frame_order.h).
            for (int o = 0; o + 1 < mult; ++o)
                fg_->recordInterpolate(slot.cmd, static_cast<uint32_t>(prev), imageIndex, order.t(o), static_cast<uint32_t>(o));
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
    if (!deferSynthesis) {
        // A deferred extrapolation job still copying its predicted frames
        // straight from the outputs (lazyOutputs_) would read what this
        // synthesis rewrites: they are dropped (it is newer anyway).
        std::unique_lock<std::mutex> wlock(workerMutex_);
        if (lazyOutputs_) {
            cancelPredicted_ = true;
            workerCv_.notify_all();
            idleCv_.wait(wlock, [this] { return !lazyOutputs_; });
            cancelPredicted_ = false;
        }
    }
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
            r = submitOn(dev_.queue, si, slot.readyFence);
            slot.readySubmitted = r >= 0;
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
        r = dev_.submit(si, deferSynthesis ? slot.readyFence : slot.fence);
        slot.readySubmitted = deferSynthesis && r >= 0;
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
    recentSlots_.push_back({slotIdx, slot.generation});
    while (recentSlots_.size() > kAppSlots) recentSlots_.pop_front();
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
        const int frames = interpolate ? mult : 1;
        const int realPos = interpolate ? order.real : 0;
        for (int p = 0; p < frames; ++p) {
            if (p == realPos) {
                dumpFrame(slot, false, 1.f);
            } else {
                const int o = p < realPos ? p : p - 1;
                dumpFrame(slot, true, order.t(o), static_cast<uint32_t>(o));
            }
        }
    }

    // Hand the presentation over to the worker.
    Job job;
    job.parity = parity_;
    job.source = imageIndex;
    job.frames = interpolate ? mult : 1;
    job.generated = interpolate;
    job.realIndex = interpolate ? order.real : 0;
    job.order = interpolate ? order : FrameOrder{};
    job.start = t0;
    job.frameStartMs = lastReturnMs_;
    job.intervalMs = frameIntervalMs_;
    // Adaptive: one output period between the frames (paceTarget spreads
    // intervalMs / frames).
    if (interpolate && adaptivePeriodMs_ > 0.0) job.intervalMs = adaptivePeriodMs_ * mult;
    job.pacing = cfg_.pacing;
    if (deferSynthesis) job.synthSlot = static_cast<int>(slotIdx);
    job.slot = static_cast<int>(slotIdx);
    job.ready = deferSynthesis ? slot.readyFence : slot.fence;
    job.staging = static_cast<int>(fg_->stagingCount());
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
    job.fifo = presentMode_ == VK_PRESENT_MODE_FIFO_KHR || presentMode_ == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    job.queuedAt = clock::now();
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
        if (interpolate) {
            const float cost = fg_->lastMeanCost();
            const float cut = fg_->lastCutWeight();  // with scene_cut_hist's gate
            if (cost >= 0.f) {
                ++statsGen_;
                statsCostSum_ += cost;
                if (cut >= 0.999f) ++statsCutFull_;
                else if (cut > 0.f) ++statsCutFade_;
            }
        }
        statsCpuMs_ += std::chrono::duration<double, std::milli>(clock::now() - t0).count();
        const auto now = clock::now();
        const double elapsed = std::chrono::duration<double>(now - statsStart_).count();
        if (elapsed >= cfg_.statsInterval) {
            const uint64_t out = statsOutFrames_.exchange(0);
            const uint64_t delayUs = statsRealDelayUs_.exchange(0);
            const uint64_t skipped = statsSkipped_.exchange(0);
            char skip[640] = "";
            int len = 0;
            if (skipped) len = snprintf(skip, sizeof(skip), ", %llu predicted frame(s) skipped", static_cast<unsigned long long>(skipped));
            if (statsStartDelayMs_ > 0.0 && statsAppFrames_ && len < int(sizeof(skip)))
                len += snprintf(skip + len, sizeof(skip) - size_t(len), ", next frame held %.1f ms",
                                statsStartDelayMs_ / statsAppFrames_);
            statsStartDelayMs_ = 0.0;
            if (cfg_.latencyLimiter && dev_.appPacesLatency(msOf(clock::now())) && len < int(sizeof(skip)))
                len += snprintf(skip + len, sizeof(skip) - size_t(len), ", latency limiter off (the game's Reflex / Anti-Lag)");
            if (statsBehind_ && len < int(sizeof(skip)))
                len += snprintf(skip + len, sizeof(skip) - size_t(len), ", %u frame(s) without generation to catch up",
                                statsBehind_);
            statsBehind_ = 0;
            if (gsLimited_.load() && len < int(sizeof(skip)))
                len += snprintf(skip + len, sizeof(skip) - size_t(len), ", gamescope frame limit %.0f fps", gsHz_.load());
            if (feedback_) {
                std::lock_guard<std::mutex> fbLock(fbMutex_);
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
                if (fbFrameLatencyCount_) {
                    // From when the game began the frame (its previous present
                    // returned: where most games read their input) to the screen.
                    char lat[64];
                    const int m = snprintf(lat, sizeof lat, ", frame-to-screen %.1f ms", fbFrameLatencySum_ / fbFrameLatencyCount_);
                    if (m > 0 && len + m < int(sizeof(skip))) { memcpy(skip + len, lat, size_t(m) + 1); len += m; }
                    fbFrameLatencySum_ = 0.0;
                    fbFrameLatencyCount_ = 0;
                }
                if (fbEverDisplayed_) {
                    char shown[48];
                    const int m = snprintf(shown, sizeof shown, ", displayed %.1f fps", double(fbDisplayed_ - statsDisplayed_) / elapsed);
                    if (m > 0 && len + m < int(sizeof(skip))) { memcpy(skip + len, shown, size_t(m) + 1); len += m; }
                }
                statsDisplayed_ = fbDisplayed_;
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
            if (statsGen_) {
                // Generated frames the scene-cut test replaced by the real
                // frame: they are displayed, but show nothing new.
                len = int(strlen(skip));
                snprintf(skip + len, sizeof(skip) - size_t(len),
                         ", scene cut: %u%% duplicated, %u%% faded (cost %.3f, thresholds %.2f/%.2f)",
                         statsCutFull_ * 100 / statsGen_, statsCutFade_ * 100 / statsGen_, statsCostSum_ / statsGen_,
                         cfg_.sceneCutLow, cfg_.sceneCutHigh);
            }
            statsGen_ = statsCutFull_ = statsCutFade_ = 0;
            statsCostSum_ = 0.0;
            BDEX_INFO("game %.1f fps -> output %.1f fps (x%.2f), present call %.2f ms, real frame delayed %.1f ms%s%s",
                      statsAppFrames_ / elapsed, out / elapsed, statsAppFrames_ ? double(out) / statsAppFrames_ : 0.0,
                      statsAppFrames_ ? statsCpuMs_ / statsAppFrames_ : 0.0,
                      statsAppFrames_ ? delayUs / 1000.0 / statsAppFrames_ : 0.0, skip, fg_->profileReport().c_str());
            statsStart_ = now;
            statsAppFrames_ = 0;
            statsCpuMs_ = 0;
        }
    }
    // max_frame_latency, like a driver's low-latency mode: the game begins
    // its next frame — and reads its input — only once the GPU has finished
    // the frame max_frame_latency presents back (its rendering and our
    // synthesis: the slot's fence). A GPU-bound game otherwise runs up to a
    // few frames ahead of the GPU, each one a frame interval of input lag.
    // Waited without mutex_ (the game may acquire meanwhile); bounded.
    // Only while generating: an armed game the control panel has not started
    // keeps the pass-through behaviour.
    VkFence latencyFence = VK_NULL_HANDLE;
    const size_t behind = static_cast<size_t>(std::max(0, cfg_.maxFrameLatency));
    if (behind > 0 && interpolate && recentSlots_.size() > behind) {
        const SlotUse& u = recentSlots_[recentSlots_.size() - 1 - behind];
        const FrameSlot& fs = slots_[u.slot];
        if (fs.generation == u.generation && fs.submitted) latencyFence = fs.fence;
    }
    lock.unlock();
    if (latencyFence) {
        const auto w0 = clock::now();
        dev_.vt.WaitForFences(dev_.device, 1, &latencyFence, VK_TRUE, 100'000'000ull);
        // (a game with Reflex or Anti-Lag on paces itself through the driver)
        const bool appPaced = dev_.appPacesLatency(msOf(clock::now()));
        if (cfg_.latencyLimiter && !appPaced) syncFrameStart(std::chrono::duration<double, std::milli>(clock::now() - w0).count());
        else startDelayMs_ = 0.0;
    } else {
        startDelayMs_ = 0.0;
    }
    lastReturnMs_ = msOf(clock::now());
    return pending;  // VK_SUCCESS or VK_SUBOPTIMAL_KHR from an earlier real present
}

// latency_limiter, after max_frame_latency's wait: the GPU has just finished
// the previous frame and still has this one to do (the game's rendering and
// our synthesis). The game's next frame only has to reach the GPU when that
// work ends; returning at once lets the game read its input a GPU frame
// earlier than needed, and that input waits in the queue. How long the game
// just waited for the GPU tells by how much it was early: the delay moves
// toward that (keeping a slack for the game's frame-time jitter), and halves
// as soon as the game finds the GPU done — it may have left it idle. A
// GPU-bound game then begins each frame just in time: the same frame rate,
// with fresher input (like Reflex or Anti-Lag). A game that is not GPU-bound
// never waits here and gets no delay.
// The slack follows the game's jitter: it grows by half each time the game
// comes back late (after the GPU went idle) and shrinks by 1 % a frame
// otherwise — a few percent of late frames at equilibrium (at least 1 ms +
// 5 % of a frame). Measured, 4K x2 with the GPU busy with our synthesis:
// frame-to-screen 26.8 -> 18.3 ms, 97 -> 101 game fps; with the CPU busy
// elsewhere (wake-ups late) it still cost 6 % of the frame rate. Holding
// only part of the wait was worse on both counts (0.6: 20.9 ms, 0.3: 24.8).
void VirtualSwapchain::syncFrameStart(double waitedMs) {
    const double interval = frameIntervalMs_;
    const bool late = waitedMs < 0.05;
    startSlackMs_ = std::clamp(startSlackMs_ * (late ? 1.5 : 0.99), 1.0 + 0.05 * interval, 0.3 * interval);
    if (late) startDelayMs_ *= 0.5;
    else startDelayMs_ += 0.5 * (waitedMs - startSlackMs_);
    startDelayMs_ = std::clamp(startDelayMs_, 0.0, 0.75 * interval);
    if (startDelayMs_ < 0.25) return;
    statsStartDelayMs_ += startDelayMs_;
    // Sleep, then spin the last half millisecond (timer slack).
    const auto until = clock::now() + std::chrono::duration_cast<clock::duration>(
                                          std::chrono::duration<double, std::milli>(startDelayMs_));
    const auto coarse = until - std::chrono::microseconds(500);
    if (coarse > clock::now()) std::this_thread::sleep_until(coarse);
    while (clock::now() < until) std::this_thread::yield();
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
        jobStartDelayMs_ = std::chrono::duration<double, std::milli>(clock::now() - job.queuedAt).count();
        runJob(job);
        pollGamescope();  // before going idle: followSurface replaces real_ only then
        {
            std::lock_guard<std::mutex> wlock(workerMutex_);
            running_ = false;
        }
        idleCv_.notify_all();
    }
}

void VirtualSwapchain::runJob(const Job& job) {
    bool submitted = false;
    auto markSubmitted = [&] {
        if (submitted) return;
        submitted = true;
        {
            std::lock_guard<std::mutex> wlock(workerMutex_);
            --unsubmitted_;
        }
        idleCv_.notify_all();
    };
    // async_compute: every semaphore a copy would have waited must be waited
    // even when that frame is dropped or fails before its copy, or it stays
    // signalled into its next use.
    std::vector<bool> copied(static_cast<size_t>(job.frames), false);
    auto consumeUnused = [&] {
        if (job.asyncSlot < 0) return;
        const FrameSlot& synth = slots_[static_cast<size_t>(job.asyncSlot)];
        std::vector<VkSemaphore> left;
        for (int k = 0; k < job.frames; ++k) {
            if (copied[size_t(k)]) continue;
            copied[size_t(k)] = true;
            const bool realK = k == job.realIndex;
            left.push_back((job.synthSlot >= 0 && realK) ? synth.realIn : synth.synthDone[k]);
        }
        if (!left.empty()) consumeSemaphores(left.data(), static_cast<uint32_t>(left.size()));
    };
    auto fail = [&](VkResult r) {
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
        {
            std::lock_guard<std::mutex> wlock(workerMutex_);
            pendingResult_ = r;
            lazyOutputs_ = false;
            acquireCv_.notify_all();
        }
        idleCv_.notify_all();
    };
    // With FIFO the presentation engine paces the frames; otherwise they are
    // spread over the game's frame interval.
    const bool paced = job.pacing && !job.fifo && job.frames > 1;

    if (job.realIndex > 0 || job.frames == 1) {
        // Interpolation, hybrid (and a lone real frame): every frame is
        // copied at once, in display order, right behind the synthesis on the
        // queue — into its swapchain image for as many as the surface lets us
        // hold acquired at once (acquireAhead_), into a staging image for the
        // rest (a surface allowing few images, high multipliers; hybrid's
        // predicted frames). The application thread only waits for these
        // copies to be submitted before it records the next synthesis (which
        // rewrites what they read), so the pacing below never holds the game
        // back; a staged frame goes to its swapchain image when presented.
        // Hybrid: the predicted frames after the real one are presented at
        // their times unless the next present's first frame is ready first.
        // (Past the staging images there are — a live change not yet
        // applied — frames are copied at their times, the game waiting.)
        const int upToReal = job.frames == 1 ? 1 : job.realIndex + 1;
        const int eager = std::min(upToReal, acquireAhead_);
        const int staged = std::min(job.frames, eager + job.staging);  // frames [eager, staged) go through staging
        std::vector<Copied> out(static_cast<size_t>(eager));
        int n = 0;
        VkResult err = VK_SUCCESS;
        for (int i = 0; i < eager; ++i) {
            const VkResult r = submitCopy(job, i, out[size_t(i)]);
            if (r < 0) { err = r; break; }
            copied[size_t(i)] = true;
            ++n;
        }
        for (int i = eager; i < staged && err >= 0; ++i) {
            const VkResult r = submitStage(job, i, static_cast<uint32_t>(i - eager));
            if (r < 0) err = r;
            else copied[size_t(i)] = true;
        }
        if (err < 0 || staged == job.frames) {
            consumeUnused();
            markSubmitted();
        }
        auto staging = [&](int i) { return i < staged ? i - eager : -1; };
        // The frames are timed from when the GPU finished the synthesis (the
        // copies follow within a fraction of a millisecond), not from the
        // game's present call: the game's rendering and the synthesis may end
        // long after that call (a GPU-bound game runs a frame ahead of the
        // GPU), and frames timed from it became ready together — the
        // generated one replaced before being displayed, x2 announced and x1
        // shown. Evenly spaced too when the GPU keeps up: the generated frame
        // went out when ready, the real one half a frame after the call.
        clock::time_point origin = clock::now();
        if (paced && n > 0) waitGpu(job.slot >= 0 ? slots_[size_t(job.slot)].fence : out[0].fence, false, origin);
        // The job's first frame went out a refresh or more after the first
        // one it could make once ready (paceTarget aims 0.3 refresh ahead):
        // the jobs before still held the refreshes — a lag that producing at
        // the display's rate never undoes (measured: a refresh more from
        // present to screen, for good, after a gamescope frame limit). Eight
        // jobs in a row (a lag that stays), and a present shows its real
        // frame alone (present()), which gives the refresh back.
        auto noteLateness = [&](int i) {
            if (!paced || i != 0) return;
            double lastMs, periodMs;
            if (lastLandMs_ <= 0.0 || !displayGrid(lastMs, periodMs)) return;
            // (Or the job itself began a refresh late: the worker was still
            // presenting the job before.)
            const double ready = msOf(origin) + 0.3 * periodMs;
            const double firstLand = lastMs + std::ceil((ready - lastMs) / periodMs) * periodMs;
            if (lastLandMs_ - firstLand > 0.9 * periodMs || jobStartDelayMs_ > 0.9 * periodMs)
                lateJobs_.fetch_add(1, std::memory_order_relaxed);
            else
                lateJobs_.store(0, std::memory_order_relaxed);
        };
        for (int i = 0; i < n; ++i) {
            if (paced) std::this_thread::sleep_until(paceTarget(job, origin, i));
            const VkResult r = presentCopied(job, out[size_t(i)]);
            if (r < 0 && err == VK_SUCCESS) err = r;
            noteLateness(i);
        }
        if (err < 0) {
            fail(err);
            return;
        }
        for (int i = eager; i < upToReal; ++i) {
            if (paced) std::this_thread::sleep_until(paceTarget(job, origin, i));
            Copied c;
            VkResult r = submitCopy(job, i, c, staging(i));
            if (r < 0) {
                fail(r);
                return;
            }
            if (i >= staged) {
                copied[size_t(i)] = true;
                if (i == job.frames - 1) markSubmitted();
            }
            r = presentCopied(job, c);
            if (r < 0) {
                fail(r);
                return;
            }
            noteLateness(i);
        }
        uint64_t dropped = 0;
        bool cancelled = false;
        for (int i = upToReal; i < job.frames; ++i) {
            if (!cancelled) cancelled = paced ? !waitUntilOrCancelled(paceTarget(job, origin, i)) : predictedCancelled();
            if (cancelled) {
                ++dropped;
                continue;
            }
            Copied c;
            VkResult r = submitCopy(job, i, c, staging(i));
            if (r < 0) {
                fail(r);
                return;
            }
            if (i >= staged) {
                copied[size_t(i)] = true;
                if (i == job.frames - 1) markSubmitted();
            }
            r = presentCopied(job, c);
            if (r < 0) {
                fail(r);
                return;
            }
        }
        if (dropped) statsSkipped_.fetch_add(dropped, std::memory_order_relaxed);
        consumeUnused();
        markSubmitted();
        return;
    }

    // Extrapolation: the real frame goes out at once...
    Copied real;
    VkResult r = submitCopy(job, 0, real);
    if (r >= 0) {
        copied[0] = true;
        if (job.frames == 1) markSubmitted();
        r = presentCopied(job, real);
    }
    // ...then the synthesis the application thread left to us (Job::synthSlot):
    // after the real frame's copy on the queue, whatever the present's
    // outcome, since the slot's fence must signal for the application thread.
    if (job.synthSlot >= 0) {
        const VkResult sr = submitDeferredSynthesis(job);
        if (r >= 0 && sr < 0) r = sr;
    }
    // The predicted frames. Synthesised by the application thread (the real
    // frame is packed: upscaling, HUD...), the next present rewrites them
    // before their time: staged now. Deferred, the next synthesis is this
    // thread's own, submitted after them: they are copied straight from the
    // outputs at their times (lazyOutputs_ tells the application thread,
    // should its next synthesis not be deferred).
    const bool lazy = job.synthSlot >= 0;
    const int staged = lazy ? 1 : std::min(job.frames, 1 + job.staging);  // frames [1, staged) go through staging
    for (int i = 1; i < staged && r >= 0; ++i) {
        r = submitStage(job, i, static_cast<uint32_t>(i - 1));
        if (r >= 0) copied[size_t(i)] = true;
    }
    if (r < 0) {
        fail(r);
        return;
    }
    if (lazy && job.frames > 1) {
        std::lock_guard<std::mutex> wlock(workerMutex_);
        lazyOutputs_ = true;
    }
    if (lazy || staged == job.frames) markSubmitted();
    // At their times after the real frame is ready on the GPU — deferred,
    // not before the synthesis is (two late frames presented together would
    // show only the second). They are obsolete once the next present's
    // real frame is ready: from then on, the ones still waiting are dropped.
    clock::time_point origin = clock::now();
    bool cancelled = job.frames > 1 && (paced ? !waitGpu(real.fence, true, origin) : predictedCancelled());
    if (!cancelled && lazy && job.frames > 1 && paced) {
        clock::time_point synthesised;
        cancelled = !waitGpu(slots_[size_t(job.synthSlot)].fence, true, synthesised);
    }
    uint64_t dropped = 0;
    for (int i = 1; i < job.frames; ++i) {
        if (!cancelled) cancelled = paced ? !waitUntilOrCancelled(paceTarget(job, origin, i)) : predictedCancelled();
        if (cancelled) {
            ++dropped;
            continue;
        }
        Copied c;
        r = submitCopy(job, i, c, i < staged ? i - 1 : -1);
        if (r < 0) {
            fail(r);
            return;
        }
        if (i >= staged) {
            copied[size_t(i)] = true;
            if (i == job.frames - 1) markSubmitted();
        }
        r = presentCopied(job, c);
        if (r < 0) {
            fail(r);
            return;
        }
    }
    if (dropped) statsSkipped_.fetch_add(dropped, std::memory_order_relaxed);
    consumeUnused();
    markSubmitted();
    if (lazy) {
        {
            std::lock_guard<std::mutex> wlock(workerMutex_);
            lazyOutputs_ = false;
        }
        idleCv_.notify_all();
    }
}

// The predicted frames still waiting are obsolete once the next present's
// first frame is ready on the GPU (it goes out then), when the application
// thread needs the images they read (cancelPredicted_), or on stop. Not as
// soon as the next present is *called*: its rendering may end a frame later
// (a GPU-bound game), and the predicted frames are ready before it — dropping
// them then showed a GPU-bound game at x1 in extrapolation and hybrid.
bool VirtualSwapchain::predictedCancelled() {
    VkFence next = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> wlock(workerMutex_);
        if (cancelPredicted_ || stop_) return true;
        if (jobs_.empty()) return false;
        next = jobs_.front().ready;
    }
    return next == VK_NULL_HANDLE || dev_.vt.GetFenceStatus(dev_.device, next) == VK_SUCCESS;
}

// Until `target`, or false as soon as predictedCancelled: sleeps until a
// newer present is queued, then watches its ready fence. (Its slot's fence
// is not recycled before the worker reaches that job: the application thread
// recycles a slot only presents later, each waiting for the job before.)
bool VirtualSwapchain::waitUntilOrCancelled(clock::time_point target) {
    VkFence next = VK_NULL_HANDLE;
    {
        std::unique_lock<std::mutex> wlock(workerMutex_);
        workerCv_.wait_until(wlock, target, [this] { return cancelPredicted_ || stop_ || !jobs_.empty(); });
        if (cancelPredicted_ || stop_) return false;
        if (jobs_.empty()) return true;  // the target, nothing newer
        next = jobs_.front().ready;
    }
    if (next == VK_NULL_HANDLE) return false;
    for (;;) {
        const auto now = clock::now();
        if (now >= target) return dev_.vt.GetFenceStatus(dev_.device, next) != VK_SUCCESS;
        // Short waits, so that cancelPredicted_ is honoured promptly.
        const int64_t left = std::chrono::duration_cast<std::chrono::nanoseconds>(target - now).count();
        const VkResult r = dev_.vt.WaitForFences(dev_.device, 1, &next, VK_TRUE, uint64_t(std::min<int64_t>(left, 1'000'000)));
        if (r != VK_TIMEOUT) return false;  // ready (or lost): obsolete
        std::lock_guard<std::mutex> wlock(workerMutex_);
        if (cancelPredicted_ || stop_) return false;
    }
}

// Bounded: a frame the GPU has not finished within 250 ms is paced from then
// (a hang is not ours to diagnose, and the pacing must not stall on it).
bool VirtualSwapchain::waitGpu(VkFence fence, bool cancellable, clock::time_point& doneAt) {
    const auto limit = clock::now() + std::chrono::milliseconds(250);
    for (;;) {
        // Short waits: vkWaitForFences returns as soon as the fence signals,
        // so doneAt stays within the driver's wake-up time of the GPU's end.
        const VkResult r = dev_.vt.WaitForFences(dev_.device, 1, &fence, VK_TRUE, 500'000);
        if (r != VK_TIMEOUT) break;
        if (cancellable && predictedCancelled()) return false;
        if (clock::now() >= limit) break;
    }
    doneAt = clock::now();
    return true;
}

// Frame k goes out k/m of the frame interval after the origin (when the GPU
// finished the job's first frame), never less than half a slot after the
// previous present (a job whose GPU work ended early must not land on the
// previous frame's refresh), and, with a known refresh grid, well before the
// refresh it should show on (a present landing just after a refresh would
// slip a whole period) and on a refresh of its own.
VirtualSwapchain::clock::time_point VirtualSwapchain::paceTarget(const Job& job, clock::time_point origin, int k) {
    const double slot = job.intervalMs / std::max(1, job.frames);
    // interp_hold: the frames up to the real one closer together (frame_order.h).
    const bool held = job.order.hold < 1.f;
    double t = msOf(origin) + (held ? job.intervalMs * job.order.offset(k) : slot * k);
    if (lastPresentMs_ > 0.0) t = std::max(t, lastPresentMs_ + 0.5 * slot * (held ? job.order.hold : 1.0));
    double lastMs, periodMs;
    if (feedback_ && displayGrid(lastMs, periodMs)) {
        double land = lastMs + std::ceil((t - lastMs) / periodMs) * periodMs;
        if (lastLandMs_ > 0.0 && land < lastLandMs_ + 0.5 * periodMs) land = lastLandMs_ + periodMs;
        t = land - 0.3 * periodMs;
    }
    return clock::time_point(std::chrono::duration_cast<clock::duration>(std::chrono::duration<double, std::milli>(t)));
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
            fbDoneId_ = std::max(fbDoneId_, entry.id);
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
                    const double frameLatency = now - entry.frameStartMs;
                    if (entry.frameStartMs > 0.0 && frameLatency >= 0.0 && frameLatency < 1000.0) {
                        fbFrameLatencySum_ += frameLatency;
                        ++fbFrameLatencyCount_;
                    }
                }
                if (fbLastMs_ > 0.0 && now - fbLastMs_ < 0.5) {
                    ++fbReplaced_;
                } else {
                    ++fbDisplayed_;
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

void VirtualSwapchain::waitDisplayed(uint64_t id, double timeoutMs) {
    std::unique_lock<std::mutex> lock(fbMutex_);
    fbIdleCv_.wait_for(lock, std::chrono::duration<double, std::milli>(timeoutMs),
                       [&] { return fbDoneId_ >= id || fbPaused_ || fbStop_ || !feedback_; });
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
        fbDoneId_ = 0;
    }
    fbCv_.notify_all();
}

double VirtualSwapchain::displayLatencyMs() {
    if (!feedback_) return -1.0;
    std::lock_guard<std::mutex> lock(fbMutex_);
    return fbLatencyMs_;
}

double VirtualSwapchain::displayHz() {
    // Under gamescope, the cycle it reports: exact, at once, and with Steam's
    // framerate limit on, the limit — what the screen shows at most. (The
    // measured grid cannot tell a 30 Hz display from a 60 Hz one we present
    // to every other refresh: at x1 it stayed at 30 Hz once the limit was
    // lifted.)
    if (const double g = gsHz_.load(); g > 0.0) return g;
    std::lock_guard<std::mutex> lock(fbMutex_);
    const double measured = displayClock_.refreshHz();
    // Not measurable (no present feedback, or no refresh grid, e.g. KDE with
    // NVIDIA): the refresh rate the control panel reads from the system.
    return measured > 0.0 ? measured : cfg_.panelDisplayHz;
}

// Worker thread, after a job (real_ stays put until it goes idle): at most
// ten times a second, the refresh cycle gamescope's WSI layer holds for our
// swapchain (it pumps gamescope's events itself) and the limiter state.
void VirtualSwapchain::pollGamescope() {
    if (!dev_.gamescope || gsFailed_) return;
    // At once when the limiter state flips: the present that got the new
    // cycle is often the one the WSI layer failed with VK_ERROR_OUT_OF_DATE_KHR
    // to have the swapchain re-created, and the next swapchain never gets it.
    const bool limited = gamescopeLimiterActive();
    const auto now = clock::now();
    if (limited == gsLimited_.load() && now - gsPolledAt_ < std::chrono::milliseconds(100)) return;
    gsPolledAt_ = now;
    VkRefreshCycleDurationGOOGLE cycle{};
    if (dev_.vt.GetRefreshCycleDurationGOOGLE(dev_.device, real_, &cycle) != VK_SUCCESS || cycle.refreshDuration == 0) {
        gsFailed_ = true;
        gsHz_ = 0.0;
        gsLimited_ = false;
        BDEX_WARN("gamescope's WSI layer gave no refresh cycle for this swapchain: measuring the display instead");
        return;
    }
    uint64_t ns = cycle.refreshDuration;
    if (gsFirstCycle_ == 0) gsFirstCycle_ = ns;
    else if (ns != gsFirstCycle_) gsCycleChanged_ = true;
    {
        // A value of its own once gamescope sent one; until then, the
        // surface's last (DeviceData::gamescopeCycles).
        std::lock_guard<std::mutex> lock(dev_.gamescopeMutex);
        uint64_t& known = dev_.gamescopeCycles[statusKey(realInfo_.surface)];
        if (gsCycleChanged_ || known == 0) known = ns;
        else ns = known;
    }
    gsHz_ = 1e9 / double(ns);
    gsLimited_ = limited;
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

// Acquires a real swapchain image and submits the copy of frame `index` of
// the job into it. Interpolation: generated frames first, the real one last.
// Extrapolation: the real frame first, then the predicted ones.
VkResult VirtualSwapchain::submitCopy(const Job& job, int index, Copied& out, int staged) {
    const bool real = index == job.realIndex;
    const uint32_t output = static_cast<uint32_t>(index < job.realIndex ? index : index - 1);

    FrameSlot& slot = nextSlot(workerSlots_, workerCursor_);
    uint32_t ri = 0;
    VkResult r = dev_.vt.AcquireNextImageKHR(dev_.device, real_, UINT64_MAX, slot.acquireSem, VK_NULL_HANDLE, &ri);
    if (r < 0) return r;
    // VK_SUBOPTIMAL is deliberately not forwarded: it reports that *our*
    // real swapchain is no longer optimal, but the application's virtual
    // swapchain is unaffected, so we must not push it into recreating (some
    // D3D translation layers hang while recreating). Instead the application
    // thread re-creates the real swapchain if the surface changed size
    // (followSurface). A genuine change fails the acquire/present with
    // OUT_OF_DATE, which we do propagate.
    if (r == VK_SUBOPTIMAL_KHR) surfaceChanged_.store(true, std::memory_order_relaxed);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dev_.vt.BeginCommandBuffer(slot.cmd, &bi);
    const uint32_t profSlot = static_cast<uint32_t>(slots_.size() + (&slot - workerSlots_.data()));
    // Without the passes' slot hooks: they belong to the application
    // thread's synthesis slots (online_tune reads back its sums there).
    fg_->profileBegin(slot.cmd, profSlot, false);
    if (staged >= 0) fg_->recordCopyStaged(slot.cmd, static_cast<uint32_t>(staged), realImages_[ri].image);
    else if (real) fg_->recordCopySource(slot.cmd, job.source, realImages_[ri].image);
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
    // (A staged copy reads what the staging copy wrote, earlier on this queue.)
    const bool async = job.asyncSlot >= 0 && staged < 0;
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
    {
        std::lock_guard<std::mutex> qlock(dev_.queueMutex(dev_.queue));
        r = dev_.vt.QueueSubmit(dev_.queue, 1, &si, slot.fence);
        if (r < 0) return r;
        // The application's present fence (VK_EXT_swapchain_maintenance1)
        // signals when its swapchain image is free to reuse. Its image is our
        // *virtual* image, freed once our copy has read it — not when the
        // real frame finishes displaying. So we signal that fence ourselves
        // after the copy (queue-ordered) instead of tying it to the real
        // present; otherwise the game blocks forever waiting on a display
        // completion that our async worker owns. A deferred synthesis still
        // reads it: submitDeferredSynthesis signals then.
        if (real && job.presentFence && job.synthSlot < 0 && staged < 0) {
            VkSubmitInfo fs{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            dev_.vt.QueueSubmit(dev_.queue, 1, &fs, job.presentFence);
        }
    }
    if (async) {
        std::lock_guard<std::mutex> wlock(workerMutex_);
        pendingCopyDone_.push_back(slot.copyDone);
    }
    slot.submitted = true;
    out.image = ri;
    out.real = real;
    out.fence = slot.fence;
    return VK_SUCCESS;
}

VkResult VirtualSwapchain::submitStage(const Job& job, int index, uint32_t stageSlot) {
    const bool real = index == job.realIndex;
    const uint32_t output = static_cast<uint32_t>(index < job.realIndex ? index : index - 1);
    FrameSlot& slot = nextSlot(workerSlots_, workerCursor_);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dev_.vt.BeginCommandBuffer(slot.cmd, &bi);
    fg_->recordStage(slot.cmd, real, output, job.source, stageSlot);
    dev_.vt.EndCommandBuffer(slot.cmd);
    // async_compute: waits the synthesis like the copy it stands for, and
    // signals when it is done reading for the next synthesis.
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSemaphore wait = VK_NULL_HANDLE;
    const bool async = job.asyncSlot >= 0;
    if (async) wait = slots_[static_cast<size_t>(job.asyncSlot)].synthDone[index];
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = async ? 1 : 0;
    si.pWaitSemaphores = &wait;
    si.pWaitDstStageMask = &stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &slot.cmd;
    si.signalSemaphoreCount = async ? 1 : 0;
    si.pSignalSemaphores = &slot.copyDone;
    VkResult r;
    {
        std::lock_guard<std::mutex> qlock(dev_.queueMutex(dev_.queue));
        r = dev_.vt.QueueSubmit(dev_.queue, 1, &si, slot.fence);
        if (r >= 0 && real && job.presentFence && job.synthSlot < 0) {  // the application's image is read
            VkSubmitInfo fs{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            dev_.vt.QueueSubmit(dev_.queue, 1, &fs, job.presentFence);
        }
    }
    if (r < 0) return r;
    if (async) {
        std::lock_guard<std::mutex> wlock(workerMutex_);
        pendingCopyDone_.push_back(slot.copyDone);
    }
    slot.submitted = true;
    return VK_SUCCESS;
}

// A present's frames past what the real swapchain lets the worker hold
// acquired at once are staged (runJob): one internal image each. Hybrid and
// extrapolation stage their predicted frames too (all but the real one).
void VirtualSwapchain::prepareStaging() {
    if (!fg_) return;
    const int frames = generating_ ? std::max(1, cfg_.multiplier) : 1;
    const bool predicted = cfg_.extrapolate || cfg_.hybrid;
    fg_->ensureStaging(static_cast<uint32_t>(std::max(0, predicted ? frames - 1 : frames - acquireAhead_)));
}

VkResult VirtualSwapchain::presentCopied(const Job& job, const Copied& c) {
    uint32_t ri = c.image;
    VkPresentIdKHR presentId{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
    VkSwapchainPresentModeInfoEXT presentMode{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT};
    const VkPresentModeKHR presentModeValue = static_cast<VkPresentModeKHR>(job.presentMode);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &realImages_[ri].readySem;
    pi.swapchainCount = 1;
    pi.pSwapchains = &real_;
    pi.pImageIndices = &ri;
    const void** tail = bdexNext(pi);
    uint64_t feedbackId = 0;
    if (c.real && job.presentId) {
        presentId.swapchainCount = 1;
        presentId.pPresentIds = &job.presentId;
        *tail = &presentId;
        tail = bdexNext(presentId);
    } else if (feedback_ && !job.presentId) {
        // Our own ids (the game cannot use present ids: they are hidden).
        feedbackId = ++presentCounter_;
        presentId.swapchainCount = 1;
        presentId.pPresentIds = &feedbackId;
        *tail = &presentId;
        tail = bdexNext(presentId);
    }
    if (job.presentMode >= 0) {
        presentMode.swapchainCount = 1;
        presentMode.pPresentModes = &presentModeValue;
        *tail = &presentMode;
        tail = bdexNext(presentMode);
    }
    // Steam's framerate limit (gamescope): gamescope shows one frame per
    // cycle of the limit and queues the others, first in first out — a
    // frame goes out once the previous one is on screen, so one frame waits
    // there instead of the whole swapchain (measured at 30 fps: 92 ms from
    // present to screen with three queued). Bounded, should the feedback stall.
    if (feedbackId > 1 && gsLimited_.load()) {
        const double hz = gsHz_.load();
        waitDisplayed(feedbackId - 1, hz > 0.0 ? 3000.0 / hz : 100.0);
    }
    const auto now = clock::now();
    if (c.real)
        statsRealDelayUs_.fetch_add(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - job.start).count()),
            std::memory_order_relaxed);
    VkResult r;
    {
        std::lock_guard<std::mutex> qlock(dev_.queueMutex(dev_.queue));
        r = dev_.vt.QueuePresentKHR(dev_.queue, &pi);
    }
    // For the spacing of the next present (paceTarget): when this one went
    // out, and the refresh it was due to show on.
    lastPresentMs_ = msOf(now);
    double lastMs, periodMs;
    lastLandMs_ = feedback_ && displayGrid(lastMs, periodMs)
                      ? lastMs + std::ceil((lastPresentMs_ - lastMs) / periodMs) * periodMs
                      : 0.0;
    if (feedbackId && r >= 0) {
        {
            std::lock_guard<std::mutex> lock(fbMutex_);
            if (!fbPaused_) fbPending_.push_back({feedbackId, msOf(job.start), job.frameStartMs, c.real});
        }
        fbCv_.notify_one();
    }
    if (r == VK_SUBOPTIMAL_KHR) surfaceChanged_.store(true, std::memory_order_relaxed);
    if (r < 0) return r;
    statsOutFrames_.fetch_add(1, std::memory_order_relaxed);
    return VK_SUCCESS;
}

} // namespace bdex
