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
#include "pacing.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
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
    bool init(HWND overlay, int gpuIndex, bool preferFifo, const Config& cfg, const std::string& gpuName = {});
    void shutdown();

    // LUID of the selected GPU, for creating the D3D11 device on the same
    // adapter (cross-adapter sharing is slow at best).
    bool luid(LUID& out) const;

    // (Re)creates the swapchain at the overlay size (the display extent).
    bool setSize(uint32_t w, uint32_t h);

    // HDR: the whole chain in scRGB (FP16 capture, D3D11 ring, import,
    // history, an EXTENDED_SRGB_LINEAR swapchain), for a game on an HDR
    // display. hdrCapable() says whether this GPU, driver and overlay surface
    // can do it; setHdr() is called before the first setSize() and before the
    // capture starts (which must use the matching format).
    bool hdrCapable() const { return hdrCapable_; }
    void setHdr(bool on);
    bool hdr() const { return hdr_; }

    // A new captured frame: import if needed, copy into the history, run the
    // analysis / flow / synthesis. `consumed` tells whether a submit acquired
    // and released the frame's keyed mutex (main forwards it to
    // Capture::recycle). 0 = ok (a job is ready), 1 = swapchain out of date,
    // -1 = fatal.
    int synthesize(const Capture::Frame& frame, bool& consumed);

    // The presentation job produced by the last synthesize().
    struct Job {
        int frames = 1;             // presents in this job (1 or multiplier)
        int realIndex = 0;          // display position of the real frame (frame_order.h): 0 extrapolation
        clock::time_point start;    // arrival of the real frame
        double intervalMs = 16.7;   // estimated capture interval
    };
    const Job& job() const { return job_; }
    // Presents frame `index` of the current job. 0 = ok, 1 = swapchain out of
    // date (resize and retry), -1 = fatal. A suboptimal swapchain still
    // presents (0): see suboptimal().
    int present(int index);
    // The pacing origin: waits (bounded) for the GPU to finish the last
    // synthesis (interpolation: before the generated frames) or the last
    // present's copy (extrapolation: the real frame), and returns when it
    // did. The frames of a job are timed from there, not from the capture:
    // the GPU, shared with the game, may end the work much later, and frames
    // timed from their submission then reach the display together.
    // `cancel` (optional): polled while waiting; true gives up (returns now).
    clock::time_point waitSynthesis();
    clock::time_point waitLastCopy(bool (*cancel)(void*) = nullptr, void* ctx = nullptr);
    // A present reported VK_SUBOPTIMAL_KHR since the last call; the swapchain
    // only needs re-creating when the window's size changed (sizeChanged()).
    bool takeSuboptimal() { const bool s = suboptimal_; suboptimal_ = false; return s; }
    bool sizeChanged() const;

    // HUD figures (captured fps, output fps).
    void setHud(int gameFps, int outFps);
    // The refresh rate of the monitor the overlay is on (target_fps = auto,
    // adaptive); 0 when unknown.
    void setDisplayHz(double hz) { displayHz_ = hz; }
    // Live status for the launcher's "En cours" view and bdex-framegen
    // tools (layer/src/status.h): `target` is the captured game.
    void publishStatus(const std::string& target, double captureFps, double outFps, double latencyMs);
    // The status published last, in French, a line per point (the tray).
    std::vector<std::string> statusLines() const;

    void setCapture(Capture* c) { cap_ = c; }

    // Runtime on/off of frame generation (the global hotkey). Off = the real
    // captured frames pass through unchanged (as if multiplier 1): no flow, no
    // in-between frames, minimal latency. Toggled from the message loop, the
    // same thread that calls synthesize(), so a plain bool is enough.
    bool toggleGeneration() { genEnabled_ = !genEnabled_; return genEnabled_; }
    void setGeneration(bool on) { genEnabled_ = on; }
    bool generation() const { return genEnabled_; }

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
    std::string lastStatus_;  // the status file text of the last publishStatus
    std::unique_ptr<FrameGen> fg_;
    static constexpr uint32_t kHistory = 3;
    AllocatedImage history_[kHistory];
    uint32_t captureW_ = 0, captureH_ = 0;  // history / engine render extent
    uint32_t engineW_ = 0, engineH_ = 0;    // display extent the engine was built for
    VkFence lastSynthFence_ = VK_NULL_HANDLE;  // the last synthesize()'s submission
    VkFence lastCopyFence_ = VK_NULL_HANDLE;   // the last present()'s copy
    bool suboptimal_ = false;
    clock::time_point waitFence(VkFence fence, bool (*cancel)(void*), void* ctx);
    int prev_ = -1;                          // history index of the previous frame
    uint32_t cur_ = 0;                       // history index of the last frame
    uint32_t parity_ = 0;
    uint64_t frameNo_ = 0;
    bool generated_ = false;                 // the last job has generated frames
    bool genEnabled_ = true;                  // frame generation on (global hotkey toggle)
    Job job_;
    clock::time_point lastArrival_{};
    pacing::IntervalFilter intervalFilter_;
    double intervalMs_ = 16.7;
    double adaptiveAcc_ = 0.0;              // adaptive: fractional frames carried over
    double displayHz_ = 0.0;                // the overlay's monitor refresh rate (0 unknown), for target_fps = auto
    int hudGame_ = 0, hudOut_ = 0;

    VkCommandPool cpool_ = VK_NULL_HANDLE;
    static constexpr uint32_t kCmdSlots = 6;
    CmdSlot cmds_[kCmdSlots];
    uint32_t cmdCursor_ = 0;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    Capture* cap_ = nullptr;
    VkFormat scFormat_ = VK_FORMAT_B8G8R8A8_UNORM;   // also the capture ring / import / history format
    VkColorSpaceKHR scColorSpace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    bool hdrCapable_ = false;
    bool hdr_ = false;
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    bool preferFifo_ = false;
    std::vector<VkImage> scImages_;
    std::vector<VkSemaphore> semDone_;  // one per swapchain image (present wait)
    uint32_t width_ = 0, height_ = 0;   // display extent

    SlotVk slots_[Capture::kRingSize];
};

} // namespace bdex
