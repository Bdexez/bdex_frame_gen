#pragma once
#include "device.h"
#include "budget.h"
#include "display_clock.h"
#include "framegen.h"

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
        uint64_t generation = 0;
        bool submitted = false;
    };
    // One application present, handed to the worker thread.
    struct Job {
        uint32_t parity = 0;
        uint32_t source = 0;         // virtual image holding the real frame
        int frames = 1;              // presents to perform (multiplier, or 1)
        bool generated = false;      // frames-1 generated images in out_[]
        bool realFirst = false;      // extrapolation: the real frame goes out first
        clock::time_point start{};   // when the application presented
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
        bool pacing = true;          // the pacing setting, carried so the worker never reads the config
    };

    void createReal(const VkSwapchainCreateInfoKHR& appInfo);
    void createRealImages();
    void destroyRealImages(std::vector<RealImage>& images);
    // Re-creates the real swapchain (and the output stage) at the surface's
    // current extent when it no longer matches; application thread, mutex_ held.
    void followSurface();
    void applyGpuBudget();  // gpu_budget: re-evaluates the quality level about once a second
    std::unique_ptr<FrameGen> makeFrameGen(bool& generating);
    void createVirtualImages(const VkSwapchainCreateInfoKHR& appInfo, bool forGeneration);
    void createSlots(std::vector<FrameSlot>& slots, uint32_t count, bool withSemaphore, VkCommandPool pool);
    void destroySlots(std::vector<FrameSlot>& slots, VkCommandPool pool);
    void destroyAll();
    FrameSlot& nextSlot(std::vector<FrameSlot>& slots, uint32_t& cursor);
    VkResult consumeSemaphores(const VkSemaphore* sems, uint32_t count);
    void dumpFrame(FrameSlot& slot, bool generated, float t, uint32_t output = 0);

    // Worker thread.
    void workerLoop();
    void runJob(const Job& job);
    VkResult presentOne(const Job& job, int index, const std::function<void()>* onSubmitted);
    VkResult submitDeferredSynthesis(const Job& job);

    // Present feedback (DeviceData::presentFeedback): every present of the
    // real swapchain carries an id; a thread waits for each with
    // vkWaitForPresentKHR and records when it was displayed, which teaches
    // displayClock_ the refresh grid. Paused while the real swapchain is
    // re-created, stopped before it is destroyed.
    void feedbackLoop();
    void pauseFeedback();
    void resumeFeedback();
    double displayHz();                 // measured refresh rate, 0 if unknown (VRR, no feedback)
    bool displayGrid(double& lastMs, double& periodMs);  // the refresh grid, if known
    static double msOf(clock::time_point t) {
        return std::chrono::duration<double, std::milli>(t.time_since_epoch()).count();
    }
    std::atomic<bool> feedback_{false};
    uint64_t presentCounter_ = 0;       // worker thread (reset while it is idle)
    std::thread fbThread_;
    std::mutex fbMutex_;
    std::condition_variable fbCv_;      // id queued / stop
    std::condition_variable fbIdleCv_;  // the thread left vkWaitForPresentKHR
    struct FbEntry {
        uint64_t id;
        double presentMs;  // the game's present call (msOf(job.start))
        bool real;         // the real frame of that present (latency is measured on it)
    };
    std::deque<FbEntry> fbPending_;
    double fbLatencyMs_ = -1.0;         // fbMutex_: smoothed present-to-screen latency of real frames
    double fbLatencySum_ = 0.0;         // fbMutex_: statistics window
    uint32_t fbLatencyCount_ = 0;
    double displayLatencyMs();          // -1 when unknown
    bool fbStop_ = false, fbBusy_ = false;
    std::atomic<bool> fbPaused_{false};
    bool fbEverDisplayed_ = false;
    int fbTimeouts_ = 0;
    DisplayClock displayClock_;         // fbMutex_
    double fbLastMs_ = 0.0;             // fbMutex_: last completion
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
    // fps_limit: next allowed application present (only the presenting
    // thread touches it, before mutex_ is taken, under limitMutex_).
    std::mutex limitMutex_;
    clock::time_point limitNext_{};
    void limitFrameRate();
    double frameIntervalMs_ = 16.6;
    uint32_t statsRepeats_ = 0;  // repeated-frame counter at the last statistics line (dedup)
    GpuBudget budget_;           // gpu_budget governor
    clock::time_point budgetAt_{};
    int flowScaleFactor_ = 1;    // flow resolution divisor multiplier of the current output stage (gpu_budget)
    int dynMult_ = 0;               // target_fps: current dynamic multiplier (0 = not chosen yet)
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
    std::atomic<uint64_t> statsSkipped_{0};  // sum of (real frame present call - app present call)
    double statsCpuMs_ = 0;
    int dumpCount_ = 0;
    uint32_t dumpId_ = 0;
    bool dumping_ = false;
};

} // namespace bdex
