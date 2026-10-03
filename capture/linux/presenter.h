// Vulkan side of bdex-capture on Linux (the counterpart of the Windows
// capture's VkCtx, capture/src/vkctx.h): the captured frames (MIT-SHM
// segments, imported as host memory with VK_EXT_external_memory_host when
// the driver accepts them, copied otherwise) go into a history ring, the
// layer's FrameGen engine (unchanged) analyses them and synthesises the
// in-between / predicted frames, and the real and generated frames are
// presented in an X11 overlay window above the game. The GPU is ours to
// choose: another one than the game's renders the generation — the two-GPU
// setup of Lossless Scaling (`--gpu`).
#pragma once

// The engine's headers before Xlib's: X11 defines None, Bool, Status...
// as macros (Config::Debug::None).
#include "device.h"

#include "x11_capture.h"
#include <vulkan/vulkan_xlib.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace bdex {

class FrameGen;

class Presenter {
public:
    using clock = std::chrono::steady_clock;

    Presenter();
    ~Presenter();
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    // The GPUs that can present to `overlay`, in enumeration order.
    static std::vector<std::string> listGpus(Display* dpy, Window overlay);
    // `gpu`: "" = the best (discrete first), a number = that GPU, otherwise a
    // part of its name (case-insensitive).
    bool init(Display* dpy, Window overlay, const std::string& gpu, bool preferFifo, const Config& cfg);
    void shutdown();
    bool setSize(uint32_t w, uint32_t h);

    // Before ring slot `slot` is written again: its last reader is done.
    void waitSlot(int slot);
    // A new captured frame: history copy, analysis, flow, synthesis.
    // 0 = ok (a job is ready), 1 = swapchain out of date, -1 = fatal.
    int synthesize(const X11Capture::Frame& frame);

    struct Job {
        int frames = 1;
        int realIndex = 0;          // display position of the real frame (frame_order.h)
        clock::time_point start;
        double intervalMs = 16.7;
    };
    const Job& job() const { return job_; }
    int present(int index);
    clock::time_point waitSynthesis();
    clock::time_point waitLastCopy(bool (*cancel)(void*) = nullptr, void* ctx = nullptr);
    bool takeSuboptimal() { const bool s = suboptimal_; suboptimal_ = false; return s; }
    bool sizeChanged() const;
    bool fifo() const;
    const char* presentModeName() const;
    bool hasSwapchain() const { return swapchain_ != VK_NULL_HANDLE; }

    void setHud(int gameFps, int outFps) { hudGame_ = gameFps; hudOut_ = outFps; }
    void setDisplayHz(double hz) { displayHz_ = hz; }
    bool toggleGeneration() { genEnabled_ = !genEnabled_; return genEnabled_; }
    bool generation() const { return genEnabled_; }
    void publishStatus(const std::string& target, double captureFps, double outFps, double latencyMs);
    const std::string& gpuName() const { return gpuName_; }
    bool hostImport() const { return hostImport_; }

private:
    struct CmdSlot {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquireSem = VK_NULL_HANDLE;
    };
    struct Upload {                 // a capture ring slot on the GPU side
        AllocatedBuffer buffer;     // imported host memory, or a staging buffer
        void* mapped = nullptr;     // staging: where the frame is copied
        bool imported = false;
        uint32_t generation = UINT32_MAX;
        VkFence lastUse = VK_NULL_HANDLE;
    };

    bool prepareUpload(Upload& u, const X11Capture::Frame& f, size_t bytes);
    void destroyUpload(Upload& u);
    bool createSwapchain(uint32_t w, uint32_t h);
    void destroySwapchain();
    bool rebuildEngine(uint32_t w, uint32_t h);
    void destroyEngine();
    CmdSlot& nextCmd();
    clock::time_point waitFence(VkFence fence, bool (*cancel)(void*), void* ctx);

    VkInstance inst_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice dev_ = VK_NULL_HANDLE;
    uint32_t qf_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::string gpuName_;
    bool hostImportExt_ = false, hostImport_ = false;
    VkDeviceSize hostAlign_ = 4096;
    PFN_vkGetMemoryHostPointerPropertiesEXT getHostProps_ = nullptr;

    InstanceData layerInst_;
    DeviceData layerDev_;
    std::unique_ptr<FrameGen> fg_;
    static constexpr uint32_t kHistory = 3;
    AllocatedImage history_[kHistory];
    uint32_t captureW_ = 0, captureH_ = 0;
    uint32_t engineW_ = 0, engineH_ = 0;
    VkFence lastSynthFence_ = VK_NULL_HANDLE, lastCopyFence_ = VK_NULL_HANDLE;
    bool suboptimal_ = false;
    int prev_ = -1;
    uint32_t cur_ = 0, parity_ = 0;
    bool genEnabled_ = true;
    Job job_;
    clock::time_point lastArrival_{};
    double intervalMs_ = 16.7, adaptiveAcc_ = 0.0, displayHz_ = 0.0;
    int hudGame_ = 0, hudOut_ = 0;

    VkCommandPool cpool_ = VK_NULL_HANDLE;
    static constexpr uint32_t kCmdSlots = 8;
    CmdSlot cmds_[kCmdSlots];
    uint32_t cmdCursor_ = 0;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat scFormat_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    bool preferFifo_ = false;
    std::vector<VkImage> scImages_;
    std::vector<VkSemaphore> semDone_;
    uint32_t width_ = 0, height_ = 0;

    Upload uploads_[X11Capture::kSlots];
};

} // namespace bdex
