#pragma once
#include "device.h"
#include "budget.h"
#include "display_clock.h"
#include "frame_order.h"
#include "pacing.h"
#include "framegen.h"
#include "recorder.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace bdex {

// A swapchain as seen by the application. The application renders into
// private images we hand out from vkAcquireNextImageKHR. On present, the
// application thread submits the GPU work that copies the frame into the
// history and synthesises the intermediate frames into internal images, then
// returns; a worker thread copies those images into the real swapchain (which
// we own), paces them and presents them. The application is therefore never
// blocked by our presentation, except by the natural back-pressure of the
// presentation engine (FIFO) or of the GPU.
class VirtualSwapchain {
public:
    VirtualSwapchain(DeviceData& dev, const VkSwapchainCreateInfoKHR& appInfo);
    ~VirtualSwapchain();
    VirtualSwapchain(const VirtualSwapchain&) = delete;
    VirtualSwapchain& operator=(const VirtualSwapchain&) = delete;

    VkSwapchainKHR handle() const { return real_; }
    // The real swapchain now: re-created underneath the application's handle
    // when the surface changes size (followSurface), so the calls the layer
    // passes through with that handle (HDR metadata, display timing...) go
    // to this one.
    VkSwapchainKHR currentReal() const { return realNow_.load(); }

    VkResult getImages(uint32_t* count, VkImage* images);
    VkResult acquire(uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* index);
    VkResult release(uint32_t count, const uint32_t* indices);
    // Presents virtual image `imageIndex`. `info` is the application's present
    // info (used for wait semaphores and the pNext chain).
    VkResult present(uint32_t imageIndex, const VkPresentInfoKHR& info, bool consumeWaitSemaphores);

    // Waits for all of the layer's pending work on this swapchain.
    void waitIdle();
    // Called when the application passes this swapchain as oldSwapchain: the
    // surface is about to change hands, so finish presenting now (later
    // presents could block forever) and refuse further work.
    void retire();

private:
    using clock = std::chrono::steady_clock;

    struct VirtualImage {
        AllocatedImage img;
        bool acquired = false;
        bool held = false;  // kept as the previous frame for the next synthesis
        bool pendingRelease = false;
        uint32_t releaseSlot = 0;
        uint64_t releaseGeneration = 0;
    };
    struct RealImage {
        VkImage image = VK_NULL_HANDLE;
        VkSemaphore readySem = VK_NULL_HANDLE;
    };
    struct FrameSlot {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquireSem = VK_NULL_HANDLE;  // used by the worker's slots only
        // Synthesis slots of a deferred extrapolation: signalled when the
        // game's frame is rendered (Job::ready), before the synthesis runs.
        VkFence readyFence = VK_NULL_HANDLE;
        bool readySubmitted = false;
        uint64_t generation = 0;
        bool submitted = false;
        // async_compute (DeviceData::asyncCompute), synthesis slots: one
        // semaphore per worker present that reads the synthesis (the k-th
        // present of the job waits synthDone[k]); for a deferred synthesis the
        // game's semaphores are consumed on the presentation queue and passed
        // on to the real copy (realIn) and to the synthesis (synthIn).
        VkSemaphore synthDone[8] = {};  // (fg::kMaxFrames)
        VkSemaphore realIn = VK_NULL_HANDLE, synthIn = VK_NULL_HANDLE;
        // async_compute, worker slots: signalled by the copy, waited by the
        // next synthesis before it overwrites the images the copy read.
        VkSemaphore copyDone = VK_NULL_HANDLE;
        // Interpolation x3 and up, two queues: the first generated frame's
        // work (analysis, flow, its synthesis) goes in `cmd` and signals
        // firstFence and synthDone[0]; the other syntheses follow in cmd2
        // (slot fence). The display schedule starts when the first frame is
        // ready instead of when all of them are (Job::firstReady).
        VkCommandBuffer cmd2 = VK_NULL_HANDLE;
        VkFence firstFence = VK_NULL_HANDLE;
        bool firstSplit = false;  // this submission was split
    };
    // One application present, handed to the worker thread.
    struct Job {
        uint32_t parity = 0;
        uint32_t source = 0;         // virtual image holding the real frame
        int frames = 1;              // presents to perform (multiplier, or 1)
        bool generated = false;      // frames-1 generated images in out_[]
        int realIndex = 0;           // display position of the real frame (frame_order.h): 0 extrapolation, frames-1 interpolation
        FrameOrder order;            // the present's frame order, for interp_hold's spacing (hold 1: even)
        bool fifo = false;           // the real swapchain presents in FIFO: the display paces
        int slot = -1;               // the synthesis slot (slots_) of this present
        VkFence firstReady = VK_NULL_HANDLE;  // the first generated frame's work done (FrameSlot::firstFence), or none
        clock::time_point start{};   // when the application presented
        clock::time_point queuedAt{};  // when present() handed it to the worker
        double frameStartMs = 0.0;   // when the application's previous present returned (it began this frame then)
        double intervalMs = 16.6;    // frame interval estimate for pacing
        int presentMode = -1;        // VkSwapchainPresentModeInfoEXT (-1 = none), applied to every present
        uint64_t presentId = 0;      // VkPresentIdKHR (0 = none), forwarded on the real frame
        VkFence presentFence = VK_NULL_HANDLE;  // VkSwapchainPresentFenceInfoEXT, idem
        // Extrapolation with a directly copied real frame: the synthesis
        // (recorded in slots_[synthSlot]) is submitted by the worker right
        // after the real frame's copy and present, so the real frame does
        // not wait for the analysis, flow and synthesis on the queue. -1 =
        // already submitted by the application thread.
        int synthSlot = -1;
        // Signals once this present's first frame can be shown (its synthesis,
        // or for a deferred one the game's rendering): the predicted frames of
        // the job before it that are still waiting then become obsolete.
        VkFence ready = VK_NULL_HANDLE;
        int staging = 0;             // staging images there were at the present (FrameGen::stagingCount)
        bool pacing = true;          // the pacing setting, carried so the worker never reads the config
        // async_compute: the synthesis slot whose semaphores this job's
        // presents wait (-1 = single queue, queue order suffices), and, for
        // a deferred synthesis, the copy semaphores it must wait.
        int asyncSlot = -1;
        std::vector<VkSemaphore> synthWaits;
    };

    void createReal(const VkSwapchainCreateInfoKHR& appInfo);
    void createRealImages();
    // VK_EXT_present_timing on the real swapchain (DeviceData::presentTiming):
    // the creation flag where the surface reports display times
    // (presentTimingFlags), then its refresh duration and time domain
    // (setupPresentTiming, after each creation); the feedback thread reads
    // each frame's display time (presentTimingMs).
    uint32_t presentTimingFlags(VkSurfaceKHR surface);
    void setupPresentTiming();
    bool presentTimingMs(uint64_t presentId, double& ms);
    uint32_t ptStage_ = 0;            // the present stage queried (first pixel visible / out), 0 = none on this surface
    bool ptWanted_ = false;           // the surface supports present timing (the creation flag)
    std::atomic<bool> ptActive_{false};  // the real swapchain reports display times
    uint64_t ptDomainId_ = 0;         // its time domain id (the steady clock's)
    double ptTicksPerMs_ = 1e6;       // that domain's units per millisecond
    std::atomic<double> ptRefreshHz_{0.0};  // its refresh duration, as a display clock hint (0 unknown, VRR)
    void destroyRealImages(std::vector<RealImage>& images);
    // Re-creates the real swapchain (and the output stage) at the surface's
    // current extent when it no longer matches; application thread, mutex_ held.
    void followSurface();
    void applyGpuBudget();  // gpu_budget: re-evaluates the quality level about once a second
    void rebuildOutput();   // require_app: the control panel changed the output stage's settings
    std::unique_ptr<FrameGen> makeFrameGen(bool& generating);
    void createVirtualImages(const VkSwapchainCreateInfoKHR& appInfo, bool forGeneration);
    void createSlots(std::vector<FrameSlot>& slots, uint32_t count, bool withSemaphore, VkCommandPool pool,
                     bool asyncSynthesis = false, bool asyncCopy = false);
    void destroySlots(std::vector<FrameSlot>& slots, VkCommandPool pool);
    void destroyAll();
    FrameSlot& nextSlot(std::vector<FrameSlot>& slots, uint32_t& cursor);
    VkResult consumeSemaphores(const VkSemaphore* sems, uint32_t count);
    void dumpFrame(FrameSlot& slot, bool generated, float t, uint32_t output = 0);

    // Worker thread.
    void workerLoop();
    void runJob(const Job& job);
    // A frame of a job copied into a real swapchain image, not presented yet.
    struct Copied {
        uint32_t image = 0;              // real swapchain image
        bool real = false;               // the application's frame (else a generated one)
        VkFence fence = VK_NULL_HANDLE;  // the copy's submission: signalled once the frame is ready
    };
    // Acquires a real image and submits the copy of frame `index` of the job
    // (from staging image `staged` when >= 0, see submitStage).
    VkResult submitCopy(const Job& job, int index, Copied& out, int staged = -1);
    // Copies frame `index` of the job into staging image `slot` at once: for
    // the frames the real swapchain cannot give an image to yet (acquireAhead_)
    // and predicted frames the next synthesis would rewrite before their time.
    VkResult submitStage(const Job& job, int index, uint32_t slot);
    void prepareStaging();  // the staging images the current output stage may need
    // Presents a copied frame (present ids, feedback, statistics).
    VkResult presentCopied(const Job& job, const Copied& c);
    // The pacing origin: waits for the GPU to finish the submission of
    // `fence`, and returns when it did in `doneAt`. `cancellable`: returns
    // false as soon as the predicted frames become obsolete (predictedCancelled).
    bool waitGpu(VkFence fence, bool cancellable, clock::time_point& doneAt);
    bool predictedCancelled();
    bool waitUntilOrCancelled(clock::time_point target);  // false: cancelled first
    // When frame k of the job goes out, from the GPU completion `origin`.
    clock::time_point paceTarget(const Job& job, clock::time_point origin, int k);
    double lastPresentMs_ = 0.0;   // worker: last present call
    uint32_t surfaceMinImages_ = 1;  // the surface's minImageCount when the real swapchain was made
    int acquireAhead_ = 1;           // real images the worker may hold acquired at once (imageCount - minImageCount + 1)
    // Worker: the real swapchain an acquire or a present reported out of
    // date. Nothing is acquired from it again: the jobs queued behind the one
    // that met it fail at once (the application re-creates its swapchain).
    // Acquiring from it went past the images the surface lets us hold — the
    // validation layer counts the images of a failed present as still
    // acquired (CI, fullscreen toggles: VUID-vkAcquireNextImageKHR-surface-07783).
    VkSwapchainKHR realOutOfDate_ = VK_NULL_HANDLE;
    double lastLandMs_ = 0.0;      // worker: refresh the last present was due to show on (known grid only)
    // workerMutex_: the application thread needs the synthesis outputs a
    // deferred extrapolation job still reads (lazyOutputs_) — its next
    // synthesis is not deferred: the job drops its predicted frames.
    bool cancelPredicted_ = false;
    bool lazyOutputs_ = false;     // workerMutex_: a job copies predicted frames straight from the outputs, later
    VkResult submitDeferredSynthesis(const Job& job);

    // Present feedback (DeviceData::presentFeedback): every present of the
    // real swapchain carries an id; a thread waits for each with
    // vkWaitForPresentKHR and records when it was displayed, which teaches
    // displayClock_ the refresh grid. Paused while the real swapchain is
    // re-created, stopped before it is destroyed.
    void feedbackLoop();
    void pauseFeedback();
    void resumeFeedback();
    bool fastGame(double hz);           // skip_fast: the game already at (nearly) the refresh rate
    double fastGameHz();                // the refresh rate skip_fast uses
    bool variableRefresh();
    void sleepUntilPrecise(clock::time_point target);             // vrr_hold: displayed when presented (no refresh grid)
    double displayHz();                 // measured refresh rate, 0 if unknown (VRR, no feedback)
    bool displayGrid(double& lastMs, double& periodMs);  // the refresh grid, if known
    static double msOf(clock::time_point t) {
        return std::chrono::duration<double, std::milli>(t.time_since_epoch()).count();
    }
    uint8_t hudGraphRing_[FgHud::kHudGraph] = {};  // HUD level 6 samples (FgHud::graph)
    int hudGraphHead_ = 0;
    uint64_t hudReplacedSeen_ = 0;
    double uncertainPct_ = -1.0;        // flow_confidence: share of uncertain flow, last statistics period
    pacing::FastGate fastGate_;         // skip_fast (pacing.h)
    std::atomic<bool> feedback_{false};
    uint64_t presentCounter_ = 0;       // worker thread (reset while it is idle)
    std::thread fbThread_;
    std::mutex fbMutex_;
    std::condition_variable fbCv_;      // id queued / stop
    std::condition_variable fbIdleCv_;  // the thread left vkWaitForPresentKHR
    struct FbEntry {
        uint64_t id;
        double presentMs;     // the game's present call (msOf(job.start))
        double frameStartMs;  // when the game began that frame (its previous present returned), 0 unknown
        bool real;            // the real frame of that present (latency is measured on it)
    };
    std::deque<FbEntry> fbPending_;
    double fbLatencyMs_ = -1.0;         // fbMutex_: smoothed present-to-screen latency of real frames
    double fbLatencySum_ = 0.0;         // fbMutex_: statistics window
    uint32_t fbLatencyCount_ = 0;
    double fbFrameLatencySum_ = 0.0;    // fbMutex_: frame start (input) to screen, statistics window
    uint32_t fbFrameLatencyCount_ = 0;
    uint64_t fbDisplayed_ = 0;          // fbMutex_: frames really displayed (not replaced)
    uint64_t statsDisplayed_ = 0;
    uint64_t hudDisplayed_ = 0;         // present thread: displayed-frame count at the last HUD rate update
    clock::time_point hudRateAt_{};
    int hudDisplayedFps_ = -1;          // displayed frames per second for the HUD, -1 unknown
    double displayLatencyMs();          // -1 when unknown
    void publishStatus(int multiplier);  // status.h entry for the control panel (present thread)
    std::chrono::steady_clock::time_point statusAt_{};
    std::string genFailReason_;         // why generation is unavailable on this swapchain (status)
    bool fbStop_ = false, fbBusy_ = false;
    std::atomic<bool> fbPaused_{false};
    bool fbEverDisplayed_ = false;
    int fbTimeouts_ = 0;
    DisplayClock displayClock_;         // fbMutex_
    double clockHintHz_ = 0.0;          // present thread: the refresh rate given to displayClock_ (gamescope's, else the panel's)
    // gamescope (gamescope.h): the refresh cycle its WSI layer reports for
    // the real swapchain — the display's, or with Steam's framerate limit on,
    // the limit's — polled by the worker after its presents; 0 unknown.
    void pollGamescope();
    std::atomic<double> gsHz_{0.0};
    std::atomic<bool> gsLimited_{false};  // Steam's framerate limit is on: gsHz_ is the limit
    clock::time_point gsPolledAt_{};      // worker
    bool gsFailed_ = false;               // worker: the WSI layer did not answer for this swapchain
    uint64_t gsFirstCycle_ = 0;           // worker: the cycle the WSI layer gave first (its default until an event)
    bool gsCycleChanged_ = false;         // worker: it has changed since — gamescope sent this swapchain one
    bool gsLimitLogged_ = false;          // present thread: the limit state last logged
    double fbLastMs_ = 0.0;             // fbMutex_: last completion
    uint64_t fbDoneId_ = 0;             // fbMutex_: last present id the thread is done with (shown, or given up)
    // Worker: returns once present `id` is done with (fbDoneId_), feedback
    // stops, or `timeoutMs` passes (fbIdleCv_).
    void waitDisplayed(uint64_t id, double timeoutMs);
    uint64_t fbReplaced_ = 0;           // fbMutex_: frames replaced before being displayed (mailbox)
    uint64_t statsReplaced_ = 0;
    void waitWorkerSubmitted(std::unique_lock<std::mutex>& lock);
    void waitWorkerIdle(std::unique_lock<std::mutex>& lock);

    DeviceData& dev_;
    // This swapchain's settings: a snapshot of dev_.config refreshed at each
    // present (DeviceData::snapshotConfig), so another thread's live reload
    // never changes them mid-frame. The worker thread does not read it.
    Config cfg_;
    uint64_t cfgVersion_ = UINT64_MAX;
    std::mutex mutex_;   // application-side state (acquire / present)
    std::condition_variable acquireCv_;  // virtual image released by present or application
    VkSwapchainKHR real_ = VK_NULL_HANDLE;
    std::atomic<VkSwapchainKHR> realNow_{VK_NULL_HANDLE};  // real_, for other threads (currentReal)
    VkSwapchainCreateInfoKHR realInfo_{};  // how the real swapchain was created (pNext not kept)
    std::atomic<bool> surfaceChanged_{false};  // the worker saw VK_SUBOPTIMAL_KHR on the real swapchain
    bool historyLost_ = false;   // output stage rebuilt: the previous frame's analysis is gone
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent_{};         // render resolution (what the application renders into)
    VkExtent2D displayExtent_{};  // real swapchain resolution (== extent_ unless upscaling)
    bool upscaling_ = false;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;  // for the per-surface Wayland upscaling state
    bool throwaway_ = false;      // native-Wayland: this swapchain exists only to learn the
                                  // window size; its first acquire returns OUT_OF_DATE so the
                                  // app recreates at the reduced size the caps hook now reports
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;

    std::vector<VirtualImage> virtualImages_;
    std::vector<RealImage> realImages_;
    // Command pools are single-threaded: one for the application thread's
    // slots, one for the worker's (both used to share the device's pool).
    VkCommandPool appPool_ = VK_NULL_HANDLE;
    VkCommandPool workerPool_ = VK_NULL_HANDLE;
    // async_compute: copy-done semaphores of submitted worker presents not yet
    // waited by a synthesis (workerMutex_).
    std::vector<VkSemaphore> pendingCopyDone_;
    VkResult submitOn(VkQueue queue, const VkSubmitInfo& si, VkFence fence);
    void waitQueuesIdle();
    std::vector<FrameSlot> slots_;        // application thread: synthesis submissions
    std::vector<FrameSlot> workerSlots_;  // worker thread: copy + present submissions
    uint32_t slotCursor_ = 0;
    uint32_t workerCursor_ = 0;
    uint32_t acquireCursor_ = 0;

    std::unique_ptr<FrameGen> fg_;
    bool retired_ = false;
    bool generating_ = false;   // frame generation active for this swapchain
    int prevIndex_ = -1;         // virtual image of the previous frame (held), -1 before the first present
    uint32_t parity_ = 0;

    // Worker state (guarded by workerMutex_).
    std::thread worker_;
    std::mutex workerMutex_;
    std::condition_variable workerCv_;   // job available / stop
    std::condition_variable idleCv_;     // job submitted / finished
    std::deque<Job> jobs_;               // queued, not yet started
    uint32_t unsubmitted_ = 0;           // queued or running jobs whose GPU work is not fully submitted
    bool running_ = false;
    bool stop_ = false;
    VkResult pendingResult_ = VK_SUCCESS;  // surfaced to the application on its next call

    clock::time_point lastAppPresent_{};
    double lastReturnMs_ = 0.0;  // when present() last returned to the game (its next frame began then)
    // max_frame_latency: the synthesis slot (and its generation) of each of
    // the last frames, newest last; present() waits for the GPU to finish
    // the frame max_frame_latency presents back before it returns.
    struct SlotUse { uint32_t slot; uint64_t generation; };
    std::deque<SlotUse> recentSlots_;
    // latency_limiter: how long present() still holds the game once the GPU
    // has finished its previous frame (syncFrameStart; presenting thread).
    pacing::StartLimiter limiter_;    // latency_limiter: the hold and the slack left (pacing.h)
    double statsStartDelayMs_ = 0.0;  // sum over the statistics window
    void syncFrameStart(double waitedMs);
    // fps_limit at the end of present (unless the game's own sleep point
    // caps: DeviceData::capAtSleep), without mutex_.
    FrameCap frameCap_;
    void limitFrameRate();
    double frameIntervalMs_ = 16.6;
    uint32_t statsRepeats_ = 0;  // repeated-frame counter at the last statistics line (dedup)
    uint32_t statsGen_ = 0;      // presents with generated frames in the statistics window
    uint32_t statsBehind_ = 0;   // presents left without generation to let the presentation catch up
    int lastPresentFrames_ = 1;  // present thread: frames of the previous present (its multiplier)
    // Worker: consecutive jobs whose real frame went out a refresh or more
    // after its time (runJob); present() then leaves one without generation.
    std::atomic<int> lateJobs_{0};
    double jobStartDelayMs_ = 0.0;  // worker: how long the running job waited for the one before
    // Present thread: the game's own time per frame, outside present() — what
    // it could run at were it not waiting for us (the adaptive multiplier
    // under Steam's framerate limit, whose waits would otherwise feed it).
    double gameOwnMs_ = 0.0;
    uint32_t statsCutFull_ = 0;  // ... whose flow cost was above scene_cut_high (generated = copy of the real frame)
    uint32_t statsCutFade_ = 0;  // ... between scene_cut_low and scene_cut_high (partly faded to it)
    double statsCostSum_ = 0.0;
    GpuBudget budget_;           // gpu_budget governor
    clock::time_point budgetAt_{};
    uint64_t outputGen_ = 0;     // Config::rebuildGen the output stage was built with (require_app)
    int flowScaleFactor_ = 1;    // flow resolution divisor multiplier of the current output stage (gpu_budget)
    int dynMult_ = 0;               // target_fps: current dynamic multiplier (0 = not chosen yet)
    pacing::AdaptivePacer adaptive_;  // adaptive: fractional frames carried over to the next game frame
    double adaptivePeriodMs_ = 0.0; // adaptive: output period of this present (0 = not adaptive)
    int chooseMultiplier();

    // On-screen HUD: a rolling window of raw game frame intervals (ms) feeds the
    // 1% / 0.1% low figures; recomputed a few times a second while the HUD is on.
    void updateHudLows(double dtMs);
    static constexpr size_t kHudWindow = 1000;
    std::vector<float> hudIntervals_;
    size_t hudRingPos_ = 0;
    clock::time_point hudStatsAt_{};
    int hudLow1_ = 0, hudLow01_ = 0;

    // statistics
    clock::time_point statsStart_{};
    uint64_t statsAppFrames_ = 0;
    std::atomic<uint64_t> statsOutFrames_{0};
    std::atomic<uint64_t> statsRealDelayUs_{0};
    std::atomic<uint64_t> statsSkipped_{0};
    std::atomic<uint64_t> statsWakeLateUs_{0}, statsWakes_{0};  // sleepUntilPrecise: how late the worker woke  // sum of (real frame present call - app present call)
    double statsCpuMs_ = 0;
    int dumpCount_ = 0;
    uint32_t dumpId_ = 0;
    bool dumping_ = false;
    bool recording_ = false;   // this present's real frame goes to the recording
    uint64_t recSeen_ = 0;     // DeviceData::recordSerial last taken
    std::unique_ptr<FrameRecorder> recorder_;
    VkBuffer recBuf_ = VK_NULL_HANDLE;  // this present's frame, in recorder_
    bool slotDone(uint32_t slot, uint64_t generation, bool wait);
};

} // namespace bdex
