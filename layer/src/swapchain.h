#pragma once
#include "device.h"
#include "framegen.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

namespace bdex {

// A swapchain as seen by the application. The application renders into
// private images we hand out from vkAcquireNextImageKHR; on present we copy
// the frame into our history, synthesise intermediate frames and present them
// all through the real swapchain that we own.
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

private:
    struct VirtualImage {
        AllocatedImage img;
        bool acquired = false;
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
        VkSemaphore acquireSem = VK_NULL_HANDLE;
        uint64_t generation = 0;
        bool submitted = false;
    };

    void createReal(const VkSwapchainCreateInfoKHR& appInfo);
    void createVirtualImages(const VkSwapchainCreateInfoKHR& appInfo);
    void createSlots();
    void destroyAll();
    FrameSlot& nextSlot();
    void waitVirtualRelease(VirtualImage& vi);
    VkResult acquireReal(FrameSlot& slot, uint32_t* realIndex);
    VkResult submitAndPresent(FrameSlot& slot, uint32_t realIndex, const VkSemaphore* appWaits, uint32_t appWaitCount,
                              const void* presentNext);
    VkResult consumeSemaphores(const VkSemaphore* sems, uint32_t count);
    void paceBeforeNextPresent(std::chrono::steady_clock::time_point frameStart, int generatedIndex, int total);
    void updateStats(int presented, double cpuMs);
    void dumpFrame(FrameSlot& slot, bool generated, float t);

    DeviceData& dev_;
    std::mutex mutex_;
    VkSwapchainKHR real_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;

    std::vector<VirtualImage> virtualImages_;
    std::vector<RealImage> realImages_;
    std::vector<FrameSlot> slots_;
    uint32_t slotCursor_ = 0;
    uint32_t acquireCursor_ = 0;

    std::unique_ptr<FrameGen> fg_;
    bool generating_ = false;   // frame generation active for this swapchain
    bool havePrevious_ = false;  // history[1-parity] holds a valid frame
    uint32_t parity_ = 0;
    VkResult pendingResult_ = VK_SUCCESS;

    using clock = std::chrono::steady_clock;
    clock::time_point lastAppPresent_{};
    double frameIntervalMs_ = 16.6;
    // statistics
    clock::time_point statsStart_{};
    uint64_t statsAppFrames_ = 0, statsOutFrames_ = 0;
    int dumpCount_ = 0;
    uint32_t dumpId_ = 0;
    bool dumping_ = false;
    double statsCpuMs_ = 0;
};

} // namespace bdex
