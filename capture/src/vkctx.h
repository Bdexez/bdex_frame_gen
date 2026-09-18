// Vulkan side of the capture product: imports the captured D3D11 textures
// (VK_KHR_external_memory_win32 + VK_KHR_win32_keyed_mutex), copies each new
// frame into a history ring, runs the layer's FrameGen engine on it (luma
// pyramid, optical flow, interpolation / extrapolation, upscaling, HUD —
// layer/src/framegen.cpp, unchanged) and presents the real and generated
// frames in a Win32 swapchain on the overlay window.
//
// Keyed-mutex protocol (per ring slot): the capture thread copies with
// AcquireSync(k)/ReleaseSync(1); the synthesis submit chains a
// VkWin32KeyedMutexAcquireReleaseInfoKHR that acquires key 1 before the
// command buffer and releases key 0 after it, so the D3D side only reuses the
// texture once the GPU is done reading it. Ownership of the image also moves
// between VK_QUEUE_FAMILY_EXTERNAL and our queue with acquire/release
// barriers, as the external-memory chapter of the spec requires.
//
// Presentation mirrors VirtualSwapchain::presentOne: interpolation shows the
// generated frame(s) first and the real one last (half a frame of added
// latency); extrapolation shows the real frame first and the predicted ones
// after it, dropping them if the next real frame arrives early.
#pragma once

#include "capture.h"

#include <windows.h>

#include <vulkan/vulkan.h>

#include "device.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace bdex {

class FrameGen;

class VkCtx {
public:
    using clock = std::chrono::steady_clock;

    VkCtx();   // both defined in vkctx.cpp: FrameGen is incomplete here and
    ~VkCtx();  // std::unique_ptr's deleter needs it complete
    VkCtx(const VkCtx&) = delete;
    VkCtx& operator=(const VkCtx&) = delete;

    // Instance/device/pipelines/surface on `overlay`. `gpuIndex` < 0 picks
    // the best GPU (discrete first); otherwise the n-th usable one. `cfg` is
    // the frame-generation configuration (multiplier, mode, preset, HUD...).
    // False = logged reason (no usable GPU, missing extensions).
    bool init(HWND overlay, int gpuIndex, bool preferFifo, const Config& cfg);
    void shutdown();

    // LUID of the selected GPU, for creating the D3D11 device on the same
    // adapter (cross-adapter sharing is slow at best).
    bool luid(LUID& out) const;

    // (Re)creates the swapchain at the overlay size (the display extent).
    bool setSize(uint32_t w, uint32_t h);

    // A new captured frame: import if needed, copy into the history, run the
    // analysis / flow / synthesis. `consumed` tells whether a submit acquired
    // and released the frame's keyed mutex (main forwards it to
    // Capture::recycle). 0 = ok (a job is ready), 1 = swapchain out of date,
    // -1 = fatal.
    int synthesize(const Capture::Frame& frame, bool& consumed);

    // The presentation job produced by the last synthesize().
    struct Job {
        int frames = 1;             // presents in this job (1 or multiplier)
        bool realFirst = true;      // extrapolation: real frame at index 0
        clock::time_point start;    // arrival of the real frame
        double intervalMs = 16.7;   // estimated capture interval
    };
    const Job& job() const { return job_; }
    // Presents frame `index` of the current job. 0 = ok, 1 = swapchain out of
    // date (resize and retry), -1 = fatal.
    int present(int index);

    // HUD figures (captured fps, output fps).
    void setHud(int gameFps, int outFps);

    void setCapture(Capture* c) { cap_ = c; }
    bool hasSwapchain() const { return swapchain_ != VK_NULL_HANDLE; }
    const char* presentModeName() const;
    bool fifo() const;

private:
    struct SlotVk {                 // an imported capture-ring texture
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        uint32_t generation = UINT32_MAX;
    };
    struct CmdSlot {                // a command buffer + fence, round-robin
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquireSem = VK_NULL_HANDLE;
    };

    bool importSlot(SlotVk& sl, const Capture::Frame& f);
    void destroyImported(SlotVk& sl);
    bool createSwapchain(uint32_t w, uint32_t h);
    void destroySwapchain();
    bool rebuildEngine(uint32_t captureW, uint32_t captureH);
    void destroyEngine();
    CmdSlot& nextCmd();

    VkInstance inst_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice dev_ = VK_NULL_HANDLE;
    uint32_t qf_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    LUID luid_{};
    bool luidValid_ = false;

    PFN_vkGetMemoryWin32HandlePropertiesKHR getMemProps_ = nullptr;

    // The layer's device abstraction, filled from our plain device so
    // FrameGen (and DeviceData's resource helpers) work unchanged.
    InstanceData layerInst_;
    DeviceData layerDev_;
    std::unique_ptr<FrameGen> fg_;
    static constexpr uint32_t kHistory = 3;
    AllocatedImage history_[kHistory];
    uint32_t captureW_ = 0, captureH_ = 0;  // history / engine render extent
    int prev_ = -1;                          // history index of the previous frame
    uint32_t cur_ = 0;                       // history index of the last frame
    uint32_t parity_ = 0;
    uint64_t frameNo_ = 0;
    bool generated_ = false;                 // the last job has generated frames
    Job job_;
    clock::time_point lastArrival_{};
    double intervalMs_ = 16.7;
    int hudGame_ = 0, hudOut_ = 0;

    VkCommandPool cpool_ = VK_NULL_HANDLE;
    static constexpr uint32_t kCmdSlots = 6;
    CmdSlot cmds_[kCmdSlots];
    uint32_t cmdCursor_ = 0;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    Capture* cap_ = nullptr;
    VkFormat scFormat_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    bool preferFifo_ = false;
    std::vector<VkImage> scImages_;
    std::vector<VkSemaphore> semDone_;  // one per swapchain image (present wait)
    uint32_t width_ = 0, height_ = 0;   // display extent

    SlotVk slots_[Capture::kRingSize];
};

} // namespace bdex
