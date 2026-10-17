#include "swapchain.h"
#include "frame_order.h"
#include "pacing.h"
#include "platform.h"
#include "thread_priority.h"
#include "gamescope.h"
#include "online_tune.h"
#include "status.h"
#include "log.h"
#include "vk_util.h"

#ifdef __linux__
#include <sys/prctl.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>

namespace bdex {

namespace {

constexpr uint32_t kAppSlots = 4;
// VK_PRESENT_MODE_FIFO_LATEST_READY_KHR (VK_EXT/KHR_present_mode_fifo_latest_ready),
// missing from the oldest supported headers: FIFO-like (one present a vblank).
constexpr auto kPresentModeFifoLatestReady = static_cast<VkPresentModeKHR>(1000361000);
bool fifoLike(VkPresentModeKHR m) {
    return m == VK_PRESENT_MODE_FIFO_KHR || m == VK_PRESENT_MODE_FIFO_RELAXED_KHR || m == kPresentModeFifoLatestReady;
}
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
    {
        std::lock_guard<std::mutex> lk(dev_.configMutex);
        recSeen_ = dev_.recordSerial;  // (a request before this swapchain is not its own)
    }
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
    if (on && fastGate_.idle) e.reason = "fast";  // skip_fast: the game near the refresh rate
    e.mode = cfg_.modeName();
    e.multiplier = on ? multiplier : 1;
    e.gameFps = frameIntervalMs_ > 0.05 ? 1000.0 / frameIntervalMs_ : 0.0;
    e.outFps = e.gameFps * e.multiplier;
    e.refreshHz = displayHz();
    e.limitHz = gsLimited_.load() ? gsHz_.load() : 0.0;
    e.fpsLimit = cfg_.fpsLimit;
    if (cfg_.fpsLimit != 0.f) e.capPoint = dev_.capsAtSleep(msOf(clock::now())) ? "input" : "present";
    if (dev_.inst) e.engine = dev_.inst->engine;
    {
        // (2: virtual_grid — the frames on a grid of our own, one a refresh)
        std::lock_guard<std::mutex> lock(fbMutex_);
        if (feedback_) e.displayGrid = displayClock_.valid() ? 1 : vgPeriodMs_ > 0.0 ? 2 : fbDisplayed_ >= 120 ? 0 : -1;
        else if (vgPeriodMs_ > 0.0) e.displayGrid = 2;
    }
    e.latencyMs = displayLatencyMs();
    {
        const double hz = e.limitHz > 0.0 ? e.limitHz : e.refreshHz;
        fbPeriodHintMs_.store(hz > 0.0 ? 1000.0 / hz : 0.0, std::memory_order_relaxed);
        const double nowMs = msOf(clock::now());
        std::lock_guard<std::mutex> lock(fbMutex_);
        if (feedback_ && statusRateAtMs_ > 0.0 && nowMs - statusRateAtMs_ >= 500.0 && hz > 0.0) {
            const double secs = (nowMs - statusRateAtMs_) / 1000.0;
            e.shownFps = double(fbDisplayed_ - statusShownSeen_) / secs;
            e.heldPerS = double(fbHeld_ - statusHeldSeen_) / secs;
        }
        if (statusRateAtMs_ <= 0.0 || nowMs - statusRateAtMs_ >= 500.0) {
            statusShownSeen_ = fbDisplayed_;
            statusHeldSeen_ = fbHeld_;
            statusRateAtMs_ = nowMs;
        }
    }
    e.budgetLevel = budget_.level();
    e.uncertainPct = uncertainPct_;
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
                                              static_cast<uint32_t>(cfg_.frameCeiling() - 1), sources, colorSpace_,
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
    } else if (cfg_.presentMode == -2 && fifoLike(ci.presentMode)) {
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
        const uint32_t mult = static_cast<uint32_t>(cfg_.requireApp ? std::max(4, cfg_.frameCeiling()) : std::max(1, cfg_.frameCeiling()));
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

    const uint32_t ptFlag = presentTimingFlags(appInfo.surface);
    ci.flags |= ptFlag;
    VkResult r = dev_.vt.CreateSwapchainKHR(dev_.device, &ci, nullptr, &real_);
    if (r < 0 && ptFlag) {
        BDEX_WARN("real swapchain with present timing failed (%s); retrying without", vkResultName(r));
        ci.flags &= ~ptFlag;
        ptStage_ = 0;
        ptWanted_ = false;
        r = dev_.vt.CreateSwapchainKHR(dev_.device, &ci, nullptr, &real_);
    }
    if (r < 0) throw VkError(r, "vkCreateSwapchainKHR (real)");
    realNow_ = real_;
    realInfo_ = ci;
    realInfo_.pNext = nullptr;
    realInfo_.oldSwapchain = VK_NULL_HANDLE;
    createRealImages();
    setupPresentTiming();
}

uint32_t VirtualSwapchain::presentTimingFlags(VkSurfaceKHR surface) {
    ptStage_ = 0;
    ptWanted_ = false;
#ifdef VK_EXT_present_timing
    if (!dev_.presentTiming || !dev_.presentFeedback) return 0;
    if (!dev_.inst->surfaceCaps2 || !dev_.inst->vt.GetPhysicalDeviceSurfaceCapabilities2KHR) {
        BDEX_DBG("present timing: the application did not enable VK_KHR_get_surface_capabilities2");
        return 0;
    }
    VkPresentTimingSurfaceCapabilitiesEXT ptc{VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT};
    VkSurfaceCapabilities2KHR caps{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR};
    caps.pNext = &ptc;
    VkPhysicalDeviceSurfaceInfo2KHR info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR};
    info.surface = surface;
    if (dev_.inst->vt.GetPhysicalDeviceSurfaceCapabilities2KHR(dev_.physDev, &info, &caps) != VK_SUCCESS ||
        !ptc.presentTimingSupported) {
        BDEX_INFO("present timing: not supported on this surface");
        return 0;
    }
    // The first pixel becoming visible (the eye's moment), else leaving the
    // display engine.
    // (Neither — an X11 surface, a compositor: the refresh duration alone.)
    if (ptc.presentStageQueries & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT)
        ptStage_ = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT;
    else if (ptc.presentStageQueries & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT)
        ptStage_ = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
    ptWanted_ = true;
    return VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;
#else
    (void)surface;
    return 0;
#endif
}

// After the real swapchain's creation: the queue of timing results, its
// refresh duration (the display clock's hint) and the time domain matching
// the steady clock — without one, no display times.
void VirtualSwapchain::setupPresentTiming() {
    ptActive_ = false;
    ptRefreshHz_ = 0.0;
#ifdef VK_EXT_present_timing
    if (!ptWanted_ || !(realInfo_.flags & VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT) ||
        !dev_.vt.SetSwapchainPresentTimingQueueSizeEXT || !dev_.vt.GetSwapchainTimingPropertiesEXT ||
        !dev_.vt.GetSwapchainTimeDomainPropertiesEXT || !dev_.vt.GetPastPresentationTimingEXT)
        return;
    VkSwapchainTimingPropertiesEXT tp{VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT};
    uint64_t counter = 0;
    if (dev_.vt.GetSwapchainTimingPropertiesEXT(dev_.device, real_, &tp, &counter) == VK_SUCCESS && tp.refreshDuration > 0)
        ptRefreshHz_ = 1e9 / double(tp.refreshDuration);
    BDEX_INFO("present timing: refresh %s, display times %s", tp.refreshDuration ? (std::to_string(1e9 / double(tp.refreshDuration)).substr(0, 6) + " Hz").c_str() : "variable or unknown",
              ptStage_ ? "from the driver" : "not reported on this surface");
    if (!ptStage_ || dev_.vt.SetSwapchainPresentTimingQueueSizeEXT(dev_.device, real_, 64) != VK_SUCCESS) return;
    VkSwapchainTimeDomainPropertiesEXT dp{VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT};
    uint64_t domains = 0;
    if (dev_.vt.GetSwapchainTimeDomainPropertiesEXT(dev_.device, real_, &dp, &domains) != VK_SUCCESS || !dp.timeDomainCount)
        return;
    std::vector<VkTimeDomainKHR> kinds(dp.timeDomainCount);
    std::vector<uint64_t> ids(dp.timeDomainCount);
    dp.pTimeDomains = kinds.data();
    dp.pTimeDomainIds = ids.data();
    if (dev_.vt.GetSwapchainTimeDomainPropertiesEXT(dev_.device, real_, &dp, &domains) < 0) return;
#ifdef _WIN32
    const VkTimeDomainKHR want = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;  // (MSVC's steady_clock)
    ptTicksPerMs_ = platformPerformanceFrequency() / 1000.0;
#else
    const VkTimeDomainKHR want = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;  // (libstdc++'s steady_clock)
    ptTicksPerMs_ = 1e6;
#endif
    for (uint32_t i = 0; i < dp.timeDomainCount; ++i)
        if (kinds[i] == want) {
            ptDomainId_ = ids[i];
            ptActive_ = true;
        }
    if (!ptActive_) BDEX_INFO("present timing: no time domain shared with the steady clock (display times from present waits)");
#endif
}

// The display time (steady-clock ms) of present `presentId`, once the driver
// reports it (the feedback thread, after vkWaitForPresentKHR).
bool VirtualSwapchain::presentTimingMs(uint64_t presentId, double& ms) {
#ifdef VK_EXT_present_timing
    if (!ptActive_) return false;
    VkPastPresentationTimingInfoEXT info{VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT};
    info.swapchain = real_;
    VkPastPresentationTimingPropertiesEXT props{VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT};
    if (dev_.vt.GetPastPresentationTimingEXT(dev_.device, &info, &props) < 0 || !props.presentationTimingCount) return false;
    std::vector<VkPastPresentationTimingEXT> t(props.presentationTimingCount, {VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT});
    std::vector<VkPresentStageTimeEXT> stages(props.presentationTimingCount);
    for (size_t i = 0; i < t.size(); ++i) {
        t[i].presentStageCount = 1;
        t[i].pPresentStages = &stages[i];
    }
    props.pPresentationTimings = t.data();
    if (dev_.vt.GetPastPresentationTimingEXT(dev_.device, &info, &props) < 0) return false;
    bool found = false;
    for (uint32_t i = 0; i < props.presentationTimingCount; ++i)
        if (t[i].presentId == presentId && t[i].reportComplete && t[i].presentStageCount && stages[i].time &&
            t[i].timeDomainId == ptDomainId_) {
            ms = double(stages[i].time) / ptTicksPerMs_;
            found = true;
        }
    return found;
#else
    (void)presentId;
    (void)ms;
    return false;
#endif
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
    realOutOfDate_ = VK_NULL_HANDLE;  // (the worker is idle)
    fg_ = std::move(fg);
    fg_->setQualityLevel(budget_.level());
    generating_ = generating;
    historyLost_ = true;
    try {
        createRealImages();
        setupPresentTiming();
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
            if (!withSemaphore) {  // (the application thread's slots)
                VK_CHECK(dev_.allocateCommandBuffer(&s.cmd2, pool));
                VK_CHECK(dev_.vt.CreateFence(dev_.device, &fi, nullptr, &s.firstFence));
            }
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
        if (s.cmd2 && pool) dev_.vt.FreeCommandBuffers(dev_.device, pool, 1, &s.cmd2);
        if (s.fence) dev_.vt.DestroyFence(dev_.device, s.fence, nullptr);
        if (s.firstFence) dev_.vt.DestroyFence(dev_.device, s.firstFence, nullptr);  // (signalled before the fence)
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
    if (recorder_) recorder_->finish([](uint32_t, uint64_t, bool) { return true; });  // (queues idle)
    recorder_.reset();
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

// Whether submission (slot, generation) of slots_ finished on the GPU (a
// slot is reused only once its fence signalled).
bool VirtualSwapchain::slotDone(uint32_t slot, uint64_t generation, bool wait) {
    FrameSlot& s = slots_[slot];
    if (s.generation != generation || !s.submitted) return true;
    return dev_.vt.WaitForFences(dev_.device, 1, &s.fence, VK_TRUE, wait ? UINT64_MAX : 0) != VK_TIMEOUT;
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
    if (target <= 0.0) {
        if (autoTarget) return std::min(2, maxMult);
        if (fastGame(fastGameHz())) return 1;
        // refresh_fill: x4 at 30 fps on 144 Hz shows 120 frames a second on
        // 144 refreshes — one frame held two refreshes every fifth, 24 times a
        // second, and the frames' content off their display by up to half a
        // refresh. When it meets or exceeds the refresh rate too (36-37 fps
        // x4 on 144 Hz), use the adaptive pacing: independent GPU origins
        // and more output than refreshes otherwise replace frames unevenly.
        // At least two outputs per game frame, up to the allocated ceiling.
        if (cfg_.allowsRefreshFill() && frameIntervalMs_ > 0.0) {
            const double hz = displayHz(), r = hz * frameIntervalMs_ / 1000.0;  // refreshes a game frame
            if (hz > 0.0 && pacing::refreshFillFits(r, cfg_.frameCeiling())) {
                adaptivePeriodMs_ = 1000.0 / hz;
                return adaptive_.next(frameIntervalMs_, adaptivePeriodMs_, cfg_.frameCeiling());
            }
        }
        return maxMult;
    }
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
        return adaptive_.next(interval, adaptivePeriodMs_, maxMult);
    }
    dynMult_ = dynamicMultiplier(dynMult_, target * frameIntervalMs_ / 1000.0, maxMult);
    // refresh_fill with target_fps (the panel's « Auto »): the largest whole
    // multiplier under the target leaves the same hole — 30 fps on 144 Hz
    // got x4, 120 frames for 144 refreshes; 40 fps x3 — so where one frame
    // more a game frame fills it, the adaptive pacing (as for a fixed one).
    const int fillFrom = cfg_.refreshFill >= 2 ? 2 : 3;
    if (cfg_.allowsRefreshFill() && dynMult_ >= fillFrom && frameIntervalMs_ > 0.0) {
        const double r = target * frameIntervalMs_ / 1000.0;
        const int ceiling = std::min(dynMult_ + 1, cfg_.frameCeiling());
        if (pacing::refreshFillFits(r, ceiling)) {
            adaptivePeriodMs_ = 1000.0 / target;
            return adaptive_.next(frameIntervalMs_, adaptivePeriodMs_, ceiling);
        }
    }
    return dynMult_;
}

// skip_fast, for a fixed multiplier: a game already at 85 % of the screen's
// refresh rate or more gains almost nothing on screen from generated frames
// (130 fps x2 on 144 Hz: 144 shown instead of 130), and pays for them in GPU
// time and the interpolation's hold — measured, a game near the refresh rate
// even lost frames to it. Half a second past the threshold either way (85 %
// in, 75 % out) so a dip does not toggle it. Unknown refresh rate (VRR
// without feedback, no panel): left alone.
// The refresh rate skip_fast compares the game with: gamescope's cycle only
// while Steam's frame limit has never been on (once lifted, the cycle stays
// the limit's until gamescope sends a new one, which a re-created swapchain
// may never get: a 30 fps game then looked "at the screen's rate" and lost
// its generated frames for good — tests/gamescope_limit.sh); elsewhere the
// larger of the measured and the panel's rates — too low a rate idles
// generation wrongly, too high one only keeps it.
// A screen that shows frames when they are presented: the present feedback
// has seen enough frames displayed, and no fixed refresh grid in them.
bool VirtualSwapchain::variableRefresh() {
    if (!feedback_) return false;
    std::lock_guard<std::mutex> lock(fbMutex_);
    return fbDisplayed_ >= 120 && !displayClock_.valid();
}

double VirtualSwapchain::fastGameHz() {
    if (dev_.gamescope) return dev_.gamescopeLimitSeen.load(std::memory_order_relaxed) || gsLimited_.load() ? 0.0 : gsHz_.load();
    std::lock_guard<std::mutex> lock(fbMutex_);
    return std::max(displayClock_.refreshHz(), double(cfg_.panelDisplayHz));
}

bool VirtualSwapchain::fastGame(double hz) {
    bool toggled = false;
    const bool idle = fastGate_.update(cfg_.skipFast, frameIntervalMs_, hz, msOf(clock::now()), toggled);
    if (toggled)
        BDEX_INFO("game at %.0f fps on a %.0f Hz screen: generation %s (skip_fast)", 1000.0 / frameIntervalMs_, hz,
                  idle ? "idle" : "back");
    return idle;
}

// Frame-rate limiter (fps_limit), like a game's own limiter: after the frame
// is handed on, the application's present returns only at its next slot,
// which paces the game itself without making a finished frame wait. Frame
// generation works best from an even base rate at a divisor of the refresh
// rate (60 -> 120, 72 -> 144). Sleeps until ~1.5 ms before the deadline, then
// spins briefly for precision; after a late frame the schedule restarts from
// now instead of letting the game burst to catch up.
void VirtualSwapchain::limitFrameRate() {
    // auto: the refresh rate divided by the multiplier (60 on 120 Hz at x2),
    // once the present feedback knows it; no cap until then.
    const double fps = cfg_.fpsLimit == 0.f ? 0.0
                       : cfg_.fpsLimit < 0.f ? displayHz() / std::max(1, cfg_.multiplier)
                                             : double(cfg_.fpsLimit);
    dev_.capFps.store(fps, std::memory_order_relaxed);  // (for the game's own sleep point, DeviceData::capAtSleep)
    if (fps <= 0.0 || dev_.capsAtSleep(msOf(clock::now()))) return;
    frameCap_.wait(fps);
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
        BDEX_WARN("settings changed: output stage not rebuilt (%s)", e.what());
        return;
    }
    BDEX_INFO("settings changed: output stage rebuilt (x%d %s, generation %s)", cfg_.multiplier, cfg_.modeName(),
              generating_ && !cfg_.paused ? "on" : "paused");
}

VkResult VirtualSwapchain::present(uint32_t imageIndex, const VkPresentInfoKHR& info, bool consumeWaitSemaphores) {
    dev_.pollConfig();
    dev_.snapshotConfig(cfg_, cfgVersion_);
    {
        // A recording asked for (DeviceData::pollRecordRequest): the real
        // frames from this present on.
        std::lock_guard<std::mutex> lk(dev_.configMutex);
        if (dev_.recordSerial != recSeen_) {
            recSeen_ = dev_.recordSerial;
            if (recorder_) recorder_->finish([this](uint32_t s, uint64_t g, bool w) { return slotDone(s, g, w); });
            recorder_ = std::make_unique<FrameRecorder>(dev_, dev_.recordDir, dev_.recordFrames);
        }
    }
    recording_ = recorder_ && recorder_->wantsFrame();
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
    if (const double hint = gsHz_.load() > 0.0 ? gsHz_.load() : ptRefreshHz_.load() > 0.0 ? ptRefreshHz_.load() : cfg_.panelDisplayHz;
        hint != clockHintHz_) {
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

    // Frame interval estimate for pacing: EMA under jitter, new cadence
    // after two consistent intervals confirm a large rate change.
    double dt = 0.0;
    if (lastAppPresent_.time_since_epoch().count() != 0) {
        dt = std::chrono::duration<double, std::milli>(t0 - lastAppPresent_).count();
    }
    frameIntervalMs_ = intervalFilter_.update(frameIntervalMs_, dt);
    lastAppPresent_ = t0;
    if (lastReturnMs_ > 0.0) {
        const double own = msOf(t0) - lastReturnMs_;
        if (own > 0.1 && own < 500.0) gameOwnMs_ = gameOwnMs_ > 0.0 ? gameOwnMs_ * 0.8 + own * 0.2 : own;
    }

    // The presentation fell behind: the previous present's frames could not
    // even be submitted — every real image still waits for the display. With
    // generation producing frames at the display's rate, such a queue never
    // drains by itself (measured in gamescope after a 1 s frame limit: 100 ms
    // more from present to screen, for good): give the queue room to drain.
    // Under pressure this present shows only its real frame; a light job
    // with a persistent display backlog can instead omit one output below.
    const bool smoothCatchup = generating_ && !cfg_.lowLatency && cfg_.catchupSmooth && cfg_.phaseSmooth && cfg_.pacePlan &&
        !cfg_.hybrid && !cfg_.extrapolate && !cfg_.dedup && !historyLost_ && prevIndex_ >= 0;
    const bool missedPlan = smoothCatchupRequested_.exchange(false, std::memory_order_relaxed);
    const double readyLag = readyLagMs_.load(std::memory_order_relaxed);
    const bool recoverMiss = missedPlan && smoothCatchup && cfg_.frameCeiling() >= 3 &&
        adaptivePeriodMs_ > 0.0 && frameIntervalMs_ >= 3.0 * adaptivePeriodMs_ && readyLag > 0.0 &&
        pacing::catchUpFrames(3, true, workerWaitMs, frameIntervalMs_, adaptivePeriodMs_, readyLag) > 1;
    const bool lagging = lateJobs_.load(std::memory_order_relaxed) >= 8 || recoverMiss;
    if (lagging) lateJobs_.store(0, std::memory_order_relaxed);
    const bool catchUp = !cfg_.lowLatency && ((!gateOnDisplay && workerWaitMs > 0.25 * frameIntervalMs_) || lagging);
    int mult = generating_ && !catchUp ? chooseMultiplier() : 1;
    if (catchUp) {
        const double ready = readyLagMs_.load(std::memory_order_relaxed);
        // A light job held behind its predecessor on the display grid: give
        // back one refresh while keeping the intermediate movement. Under
        // worker/GPU pressure, keep the real-only path which avoids flow work.
        if (smoothCatchup &&
            lagging && workerWaitMs <= 0.25 * frameIntervalMs_) {
            const int nominal = chooseMultiplier();
            double lastGridMs, gridPeriodMs;
            if (displayGrid(lastGridMs, gridPeriodMs))
                mult = pacing::catchUpFrames(nominal, lagging, workerWaitMs, frameIntervalMs_, adaptivePeriodMs_, ready);
            if (mult > 1) statsTrimmed_ += uint32_t(nominal - mult);
        }
        if (mult == 1) ++statsBehind_;
        BDEX_DBG("catch up: keep %d output(s), worker wait %.3f ms, interval %.3f ms, persistent late %d, ready lag %.3f ms",
                 mult, workerWaitMs, frameIntervalMs_, int(lagging), ready);
        if (recoverMiss) BDEX_DBG("catch up: newly missed refresh of the presentation plan");
    }
    lastPresentFrames_ = mult;
    {
        // virtual_grid: frames one output period apart on a grid of our own
        // where the display's is not measured (Wayland compositors, KDE with
        // NVIDIA) — without one, each job's frames were spaced from its own
        // GPU completion and the jobs met unevenly: pacing simulator, 30 fps
        // on 144 Hz, 137 frames a second shown and 7 repeated, 144 and none
        // with it. A grid at the wrong phase (our presents where the
        // compositor latches) gets frames replaced: moved half a period.
        std::lock_guard<std::mutex> lock(fbMutex_);
        const double nowMs = msOf(t0);
        if (cfg_.virtualGrid && adaptivePeriodMs_ > 0.0 && !(feedback_ && displayClock_.valid())) {
            if (vgAnchorMs_ <= 0.0) vgAnchorMs_ = fbLastMs_ > 0.0 ? fbLastMs_ : nowMs;
            vgPeriodMs_ = adaptivePeriodMs_;
            if (nowMs - vgCheckMs_ >= 1000.0) {
                const uint64_t missed = fbReplaced_ + fbClose_;
                if (vgCheckMs_ > 0.0 && missed - vgReplacedSeen_ >= 3) {
                    vgAnchorMs_ += 0.5 * vgPeriodMs_;
                    BDEX_DBG("virtual grid: %llu frames replaced in a second, phase moved", (unsigned long long)(missed - vgReplacedSeen_));
                }
                vgReplacedSeen_ = missed;
                vgCheckMs_ = nowMs;
            }
        } else {
            vgPeriodMs_ = 0.0;
        }
    }
    // Where the real frame goes among the frames of this present (and, with
    // interp_hold, how far apart; adaptive keeps one output period between
    // all of them).
    FrameOrder order = FrameOrder::of(cfg_, mult, !(adaptivePeriodMs_ > 0.0));
    // Never closer than a refresh apart (also the fastest a VRR screen
    // shows): two frames inside one refresh would replace each other. At
    // 60 fps x2 on 144 Hz that is a hold of 0.83 at least; the measured
    // 0.5 showed 116 of the 120 frames, 0.25 only 95.
    // vrr_hold: a screen that shows each frame when it is presented (VRR:
    // the present feedback works but finds no refresh grid) gains nothing
    // from even spacing, and interp_hold 1 is taken as auto there.
    float askHold = cfg_.interpHold;
    if (askHold >= 1.f && cfg_.vrrHold && order.real > 0 && !(adaptivePeriodMs_ > 0.0) && variableRefresh()) {
        askHold = -1.f;
        order.hold = 0.1f;  // (FrameOrder::of's auto: raised to the bound below)
    }
    if (order.hold < 1.f && frameIntervalMs_ > 0.0)
        order.hold = pacing::effectiveHold(order.hold, askHold, displayHz(), mult, frameIntervalMs_, order.real);
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
        if (cfg_.overlayMode >= 6) {
            // The pacing graph: this present's frame time, whether it carried
            // generated frames, whether a frame was replaced before display
            // since the previous sample (present feedback).
            uint8_t sample = static_cast<uint8_t>(std::clamp(static_cast<int>(dt * 2.0 + 0.5), 0, 63));
            if (interpolate) sample |= 0x40;
            if (feedback_) {
                uint64_t replaced;
                {
                    std::lock_guard<std::mutex> fbLock(fbMutex_);
                    replaced = fbReplaced_;
                }
                if (replaced != hudReplacedSeen_) sample |= 0x80;
                hudReplacedSeen_ = replaced;
            }
            hudGraphRing_[hudGraphHead_] = sample;
            hudGraphHead_ = (hudGraphHead_ + 1) % FgHud::kHudGraph;
            uint8_t ordered[FgHud::kHudGraph];
            for (int i = 0; i < FgHud::kHudGraph; ++i) ordered[i] = hudGraphRing_[(hudGraphHead_ + i) % FgHud::kHudGraph];
            fg_->setHudGraph(ordered, FgHud::kHudGraph, std::clamp(static_cast<int>(frameIntervalMs_ * 2.0 + 0.5), 0, 63));
        }
    }

    // Synthesis: analyse the application's frame (in place, no copy) and
    // generate the intermediate frames into the internal output images.
    FrameSlot& slot = nextSlot(slots_, slotCursor_);
    const uint32_t slotIdx = static_cast<uint32_t>(&slot - slots_.data());
    // (still set: a present that failed after taking it)
    if (recording_ && !recBuf_)
        recBuf_ = recorder_->next(extent_, fg_->encoding(),
                                  [this](uint32_t s, uint64_t g, bool w) { return slotDone(s, g, w); });
    recording_ = recBuf_ != VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    // Interpolation x3 and up on two queues: the first generated frame is
    // submitted alone, ahead of the other syntheses (FrameSlot::cmd2).
    // Measured, RTX 3060 Ti 1440p x4: ~1.2 ms a synthesis, the schedule (real
    // frame included) begins ~2.4 ms earlier.
    slot.firstSplit = generating_ && interpolate && !cfg_.extrapolate && mult >= 3 && slot.cmd2 && slot.firstFence;
    dev_.vt.BeginCommandBuffer(slot.cmd, &bi);
    VkCommandBuffer cb = slot.cmd;  // (cmd2 after the first generated frame, when split)
    fg_->profileBegin(cb, slotIdx);
    fg_->collectMeanCost(slotIdx);  // the slot's fence was waited in nextSlot
    double planned[8] = {};  // Job::planned
    bool planActive = false;
    if (!generating_) motionClock_.reset();
    if (generating_) {
        fg_->recordAcquireSources(cb, prev, static_cast<int>(imageIndex));
        fg_->recordAnalysis(cb, parity_, imageIndex);
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
        const bool grid = interpolate && displayGrid(lastMs, periodMs);
        // interp_hold: the outputs read their t from these display times too,
        // so frame_timing follows the uneven spacing.
        const bool held = interpolate && order.hold < 1.f;
        if (grid || adaptive || held) {
            const double now = msOf(t0);
            auto land = [&](double ms) {
                return grid ? lastMs + std::ceil((ms - lastMs) / periodMs) * periodMs : ms;
            };
            double at[8] = {};
            // pace_plan: on a grid, each frame planned for its own refresh
            // from when the first one is usually ready, and presented for it
            // (pacing::planLands) — where the frames fit at one a refresh.
            const bool plan = grid && cfg_.pacePlan && !cfg_.extrapolate && mult <= 8 &&
                              (adaptive || mult * periodMs <= frameIntervalMs_ * 1.02);
            if (plan) {
                double base[8] = {};
                for (int k = 1; k < mult; ++k)
                    base[k] = adaptive ? spacing * k : held ? frameIntervalMs_ * order.offset(k) : frameIntervalMs_ * k / mult;
                const pacing::Grid g{true, lastMs, periodMs};
                pacing::planLands(now, readyLagMs_.load(std::memory_order_relaxed), base, mult, g, lastPlannedMs_, planned);
                for (int k = 0; k < mult; ++k) at[k] = planned[k] - now;  // (planned[] again below)
            } else if (adaptive) {
                for (int k = 0; k < mult && k < 8; ++k) at[k] = land(now + 1.0 + spacing * k) - now;
            } else {  // (historical: frame k at the refresh after start + I*k/m, frame 0 right after the GPU)
                for (int k = 0; k < mult && k < 8; ++k)
                    at[k] = (k == 0 ? land(now + 1.0)
                             : land(now + (held ? frameIntervalMs_ * order.offset(k) : frameIntervalMs_ * k / mult))) - now;
            }
            for (int k = 0; k < mult && k < 8; ++k) planned[k] = now + at[k];
            planActive = plan;
            timing.haveDisplay = true;
            timing.displayMs[7] = float(at[order.real]);
            for (int o = 0; o + 1 < mult && o < 7; ++o) timing.displayMs[o] = float(at[order.position(o)]);
        }
        timing.displaySpanMs = motionClock_.span(planActive ? planned[order.real] : 0.0, dt,
            cfg_.phaseSmooth && planActive && !cfg_.hybrid && !cfg_.extrapolate && !cfg_.dedup && !timing.reset);
        fg_->recordFrameState(cb, timing);
        fg_->profileMark(cb, slotIdx, FrameGen::StageAnalysis);
        if (interpolate) {
            fg_->recordFlow(cb, parity_, static_cast<int>(slotIdx));
            fg_->profileMark(cb, slotIdx, FrameGen::StageFlow);
            // Frames between prev and cur (t in (0,1)) before the real frame,
            // frames after cur (t in (1,2)) after it (frame_order.h).
            for (int o = 0; o + 1 < mult; ++o) {
                fg_->recordInterpolate(cb, static_cast<uint32_t>(prev), imageIndex, order.t(o), static_cast<uint32_t>(o));
                if (o == 0 && slot.firstSplit) {  // the first generated frame is complete: the rest in cmd2
                    dev_.vt.EndCommandBuffer(cb);
                    cb = slot.cmd2;
                    dev_.vt.BeginCommandBuffer(cb, &bi);
                }
            }
            fg_->profileMark(cb, slotIdx, FrameGen::StageInterp);
        }
        // Resample the real frame into the internal image the worker presents
        // (for upscaling, or so the HUD can be drawn on it).
        if (fg_->packReal()) fg_->recordUpscaleReal(cb, imageIndex, interpolate ? prev : -1);
        if (dumping_) {
            if (interpolate)
                for (int i = 0; i < mult - 1; ++i) fg_->recordDump(cb, static_cast<uint32_t>(i), imageIndex);
            fg_->recordDump(cb, fg_->outputCount(), imageIndex);
        }
        if (recBuf_) fg_->recordReadbackSource(cb, imageIndex, recBuf_);
        fg_->recordReleaseSources(cb, prev, static_cast<int>(imageIndex));
    } else if (fg_->packReal()) {
        // No frame generation: still process the one real frame (upscale / HUD).
        fg_->recordAcquireSources(cb, -1, static_cast<int>(imageIndex));
        fg_->recordUpscaleReal(cb, imageIndex);
        if (dumping_) fg_->recordDump(cb, fg_->outputCount(), imageIndex);
        if (recBuf_) fg_->recordReadbackSource(cb, imageIndex, recBuf_);
        fg_->recordReleaseSources(cb, -1, static_cast<int>(imageIndex));
    } else if (dumping_ || recording_) {
        // Ground-truth runs use multiplier=1: copy the unmodified source into
        // the dump buffer while the image is temporarily in GENERAL layout.
        fg_->recordAcquireSources(cb, -1, static_cast<int>(imageIndex));
        if (dumping_) fg_->recordDump(cb, fg_->outputCount(), imageIndex);
        if (recBuf_) fg_->recordReadbackSource(cb, imageIndex, recBuf_);
        fg_->recordReleaseSources(cb, -1, static_cast<int>(imageIndex));
    }
    // Pass-through: an empty submission still consumes the application's
    // semaphores and orders the worker's copy after its rendering.
    dev_.vt.EndCommandBuffer(cb);

    // Extrapolation presents the real frame first. When it is copied straight
    // from the application's image (no upscaling / HUD / repeat replacement,
    // which the synthesis produces), the synthesis is handed to the worker
    // and submitted after that copy: the real frame then leaves as soon as
    // the game's rendering is done instead of queueing behind the analysis,
    // flow and synthesis (their GPU time is the latency saved). Here only the
    // game's semaphores are consumed, which orders the worker's copy after
    // its rendering. Not while dumping (dumpFrame waits on the slot at once).
    const bool deferSynthesis = cfg_.extrapolate && interpolate && !fg_->packReal() && !dumping_ && !recording_;
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
            if (slot.firstSplit) {
                // The first generated frame (its present waits synthDone[0])
                // with the game's semaphores, then the rest behind it.
                si.signalSemaphoreCount = 1;
                si.pSignalSemaphores = slot.synthDone;
                dev_.vt.ResetFences(dev_.device, 1, &slot.firstFence);
                r = submitOn(dev_.computeQueue, si, slot.firstFence);
                if (r >= 0) {
                    VkSubmitInfo rest{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                    rest.commandBufferCount = 1;
                    rest.pCommandBuffers = &slot.cmd2;
                    rest.signalSemaphoreCount = static_cast<uint32_t>(frames - 1);
                    rest.pSignalSemaphores = slot.synthDone + 1;
                    r = submitOn(dev_.computeQueue, rest, slot.fence);
                }
            } else {
                si.signalSemaphoreCount = static_cast<uint32_t>(frames);
                si.pSignalSemaphores = slot.synthDone;
                r = submitOn(dev_.computeQueue, si, slot.fence);
            }
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
    if (recBuf_) recorder_->queued(slotIdx, slot.generation, frameIntervalMs_ > 0.0 ? 1000.0 / frameIntervalMs_ : 0.0, msOf(t0));
    recBuf_ = VK_NULL_HANDLE;
    if (recorder_) recorder_->pump([this](uint32_t s, uint64_t g, bool w) { return slotDone(s, g, w); });

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
    if (interpolate) std::copy(std::begin(planned), std::end(planned), std::begin(job.planned));
    job.plan = interpolate && planActive;
    job.smoothCatchup = job.plan && smoothCatchup && mult >= 3;
    // Adaptive: one output period between the frames (paceTarget spreads
    // intervalMs / frames).
    if (interpolate && adaptivePeriodMs_ > 0.0) job.intervalMs = adaptivePeriodMs_ * mult;
    job.pacing = cfg_.pacing;
    if (deferSynthesis) job.synthSlot = static_cast<int>(slotIdx);
    job.slot = static_cast<int>(slotIdx);
    job.firstReady = slot.firstSplit ? slot.firstFence : VK_NULL_HANDLE;
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
    job.fifo = fifoLike(presentMode_);
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
            if (fg_) {
                const double u = fg_->takeUncertainShare();
                if (u >= 0.0) uncertainPct_ = 100.0 * u;
                if (u >= 0.0 && len < int(sizeof(skip)))
                    len += snprintf(skip + len, sizeof(skip) - size_t(len), ", flow uncertain on %.1f%% of the picture", 100.0 * u);
            }
            if (const uint64_t wakes = statsWakes_.exchange(0); wakes && len < int(sizeof(skip)))
                len += snprintf(skip + len, sizeof(skip) - size_t(len), ", wake-up late %.0f us",
                                double(statsWakeLateUs_.exchange(0)) / double(wakes));
            if (fastGate_.idle && len < int(sizeof(skip)))
                len += snprintf(skip + len, sizeof(skip) - size_t(len), ", generation idle: the game is at the screen's rate (skip_fast)");
            if (cfg_.latencyLimiter && dev_.appPacesLatency(msOf(clock::now())) && len < int(sizeof(skip)))
                len += snprintf(skip + len, sizeof(skip) - size_t(len), ", latency limiter off (the game's Reflex / Anti-Lag)");
            if (statsBehind_ && len < int(sizeof(skip)))
                len += snprintf(skip + len, sizeof(skip) - size_t(len), ", %u frame(s) without generation to catch up",
                                statsBehind_);
            statsBehind_ = 0;
            if (statsTrimmed_ && len < int(sizeof(skip))) {
                const int written = snprintf(skip + len, sizeof(skip) - size_t(len),
                    ", %u intermediate frame(s) omitted to catch up", statsTrimmed_);
                if (written > 0) len += std::min(written, int(sizeof(skip)) - len - 1);
            }
            statsTrimmed_ = 0;
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
        else limiter_.delayMs = 0.0;
    } else {
        limiter_.delayMs = 0.0;
    }
    // fps_limit: the frame is already on its way; the game is held here,
    // before it begins the next one and reads its input — what a game's own
    // limiter does. (Holding the present itself, as until 13/10, made each
    // finished frame wait for its slot: +23 ms input to screen at a 40 fps
    // cap, x1 and x2 alike, against the game's own limiter.)
    limitFrameRate();  // (without mutex_: never block acquire while sleeping)
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
    const double delay = limiter_.step(waitedMs, frameIntervalMs_);  // (pacing.h)
    if (delay <= 0.0) return;
    statsStartDelayMs_ += delay;
    // Sleep, then spin the last half millisecond (timer slack).
    const auto until = clock::now() + std::chrono::duration_cast<clock::duration>(
                                          std::chrono::duration<double, std::milli>(delay));
    const auto coarse = until - std::chrono::microseconds(500);
    if (coarse > clock::now()) std::this_thread::sleep_until(coarse);
    while (clock::now() < until) std::this_thread::yield();
}

} // namespace bdex
