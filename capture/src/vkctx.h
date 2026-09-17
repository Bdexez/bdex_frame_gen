// Vulkan context for the capture PoC: imports the captured D3D11 textures
// (VK_KHR_external_memory_win32 + VK_KHR_win32_keyed_mutex), runs the layer's
// upscale shader from layer/shaders/upscale.comp into a packed R32_UINT stage
// image (the same pattern as framegen.cpp's outReal_), copies it into a Win32
// swapchain and presents it in the overlay window.
//
// Keyed-mutex protocol (per ring slot): the capture thread copies with
// AcquireSync(k)/ReleaseSync(1); the submit here chains a
// VkWin32KeyedMutexAcquireReleaseInfoKHR that acquires key 1 before the
// command buffer and releases key 0 after it, so the D3D side only reuses the
// texture once the GPU is done reading it. Ownership of the image also moves
// between VK_QUEUE_FAMILY_EXTERNAL and our queue with acquire/release
// barriers, as the external-memory chapter of the spec requires.
#pragma once

#include "capture.h"

#include <windows.h>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace bdex {

// Push-constant block of layer/shaders/upscale.comp: two ivec2, then seven
// 4-byte scalars (std430) = 44 bytes.
struct UpscalePC {
    int32_t outSize[2];
    int32_t srcSize[2];
    uint32_t encoding;
    int32_t filterMode;
    int32_t hudMode;
    int32_t hudGame;
    int32_t hudOut;
    int32_t hudLow1;
    int32_t hudLow01;
};
static_assert(sizeof(UpscalePC) == 44, "upscale.comp push-constant block");

// Per-frame shader parameters chosen by main.
struct DrawParams {
    int filterMode = 1;  // 0 bilinear, 1 bicubic (Catmull-Rom), 2 Lanczos-2
    int hudMode = 0;     // fps HUD detail (0 off, 1 fps, 2 capture>output, 3/4 lows)
    int hudGame = 0;     // captured fps
    int hudOut = 0;      // presented fps
};

class VkCtx {
public:
    VkCtx() = default;
    ~VkCtx() { shutdown(); }
    VkCtx(const VkCtx&) = delete;
    VkCtx& operator=(const VkCtx&) = delete;

    // Instance/device/pipeline/surface on `overlay`. False = logged reason
    // (no usable GPU: missing extensions or D3D11-import not supported).
    bool init(HWND overlay, bool preferFifo);
    void shutdown();

    // (Re)creates the swapchain and the stage image at the overlay size.
    bool setSize(uint32_t w, uint32_t h);

    // 0 = presented, 1 = swapchain out of date (resize and retry), -1 = fatal.
    // `consumed` tells whether a submit acquired/released the frame's keyed
    // mutex — main forwards it to Capture::recycle().
    int draw(const Capture::Frame& frame, const DrawParams& params, bool& consumed);

    // The capture feed, used to release graveyarded rings once their imports
    // are freed. Not owned.
    void setCapture(Capture* c) { cap_ = c; }
    bool hasSwapchain() const { return swapchain_ != VK_NULL_HANDLE; }
    const char* presentModeName() const;

private:
    struct SlotVk {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t generation = UINT32_MAX;
        bool firstUse = true;
    };

    bool importSlot(SlotVk& sl, const Capture::Frame& f);
    bool createSwapchain(uint32_t w, uint32_t h);
    void destroySwapchain();
    void destroyImported(SlotVk& sl);

    VkInstance inst_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice dev_ = VK_NULL_HANDLE;
    uint32_t qf_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;

    PFN_vkGetMemoryWin32HandlePropertiesKHR getMemProps_ = nullptr;

    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl_ = VK_NULL_HANDLE;
    VkPipelineLayout playout_ = VK_NULL_HANDLE;
    VkPipeline pipe_ = VK_NULL_HANDLE;
    VkDescriptorPool dpool_ = VK_NULL_HANDLE;
    VkDescriptorSet dset_ = VK_NULL_HANDLE;
    VkCommandPool cpool_ = VK_NULL_HANDLE;

    static constexpr uint32_t kFrames = 2;
    VkCommandBuffer cbs_[kFrames] = {};
    VkSemaphore semImg_[kFrames] = {};
    VkFence fences_[kFrames] = {};
    uint64_t frameNo_ = 0;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    Capture* cap_ = nullptr;
    VkFormat scFormat_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    bool preferFifo_ = false;
    std::vector<VkImage> scImages_;
    std::vector<VkSemaphore> semDone_;  // one per swapchain image (present wait)

    VkImage stage_ = VK_NULL_HANDLE;
    VkDeviceMemory stageMem_ = VK_NULL_HANDLE;
    VkImageView stageView_ = VK_NULL_HANDLE;
    uint32_t width_ = 0, height_ = 0;

    SlotVk slots_[Capture::kRingSize];
};

} // namespace bdex
