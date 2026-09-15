#pragma once
#include "device.h"
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
    };

    void createReal(const VkSwapchainCreateInfoKHR& appInfo);
    void createRealImages();
    void destroyRealImages(std::vector<RealImage>& images);
    // Re-creates the real swapchain (and the output stage) at the surface's
    // current extent when it no longer matches; application thread, mutex_ held.
    void followSurface();
    std::unique_ptr<FrameGen> makeFrameGen(bool& generating);
    void createVirtualImages(const VkSwapchainCreateInfoKHR& appInfo, bool forGeneration);
    void createSlots(std::vector<FrameSlot>& slots, uint32_t count, bool withSemaphore);
    void destroySlots(std::vector<FrameSlot>& slots);
    void destroyAll();
    FrameSlot& nextSlot(std::vector<FrameSlot>& slots, uint32_t& cursor);
    void waitVirtualRelease(VirtualImage& vi);
    VkResult consumeSemaphores(const VkSemaphore* sems, uint32_t count);
    void dumpFrame(FrameSlot& slot, bool generated, float t);

    // Worker thread.
    void workerLoop();
    void runJob(const Job& job);
    VkResult presentOne(const Job& job, int index, const std::function<void()>* onSubmitted);
    void waitWorkerSubmitted(std::unique_lock<std::mutex>& lock);
    void waitWorkerIdle(std::unique_lock<std::mutex>& lock);

    DeviceData& dev_;
    std::mutex mutex_;   // application-side state (acquire / present)
    VkSwapchainKHR real_ = VK_NULL_HANDLE;
    VkSwapchainCreateInfoKHR realInfo_{};  // how the real swapchain was created (pNext not kept)
    std::atomic<bool> surfaceChanged_{false};  // the worker saw VK_SUBOPTIMAL_KHR on the real swapchain
    bool historyLost_ = false;   // output stage rebuilt: the previous frame's analysis is gone
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};         // render resolution (what the application renders into)
    VkExtent2D displayExtent_{};  // real swapchain resolution (== extent_ unless upscaling)
    bool upscaling_ = false;
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;

    std::vector<VirtualImage> virtualImages_;
    std::vector<RealImage> realImages_;
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
    double frameIntervalMs_ = 16.6;

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
