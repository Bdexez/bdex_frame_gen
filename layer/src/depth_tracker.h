#pragma once
// The game's depth buffer (the `depth` option): which of its depth images it
// draws its scene with, and that image's state when a frame is presented —
// read from the game's own Vulkan commands, as ReShade's generic depth add-on
// does for D3D and Vulkan games. Under Proton it sees DXVK's and
// VKD3D-Proton's commands, so D3D games are covered too.
//
// The layer only watches these commands when `depth` is on at the device's
// creation (layer.cpp returns these hooks then, and only then): image and
// view creation, framebuffers and render passes, the start of every render
// pass (vkCmdBeginRenderPass*, vkCmdBeginRendering*), image barriers and
// depth clears. A command buffer's events are applied when it is submitted,
// in submission order (games record command buffers on several threads, in
// any order): the per-image layout and clear value then are those the GPU
// will have reached when the layer's own work, submitted at the present on
// the same queue, runs.
//
// Choosing (choose(), pure): among the depth images a frame used, those
// covering the picture's proportions (a shadow map is square, a cube face
// too) and at least 30 % of its width (a render resolution under DLSS/FSR
// ultra performance is 33 %); the most used one — passes times area — wins,
// the previous pick staying while it keeps half the leader's score.
#include "vk_compat.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace bdex {

struct DeviceData;

// Non-dispatchable handles are pointers on 64-bit builds and uint64_t on
// 32-bit ones: as integers, either way (no reinterpret_cast on Win32).
template <typename H>
inline uint64_t handleBits(H h) {
    if constexpr (std::is_pointer_v<H>) return uint64_t(reinterpret_cast<uintptr_t>(h));
    else return uint64_t(h);
}

template <typename H>
inline H handleFromBits(uint64_t v) {
    if constexpr (std::is_pointer_v<H>) return reinterpret_cast<H>(uintptr_t(v));
    else return H(v);
}

namespace depth {

bool isDepthFormat(VkFormat f);
bool hasStencil(VkFormat f);

// One depth image's use during the frame, for choose().
struct Usage {
    uint64_t image = 0;
    VkExtent2D extent{};
    VkRect2D area{};       // the largest area drawn into it
    uint32_t passes = 0;   // render passes that used it as depth attachment
    bool sampleable = false;  // single-sampled 2D image with SAMPLED usage
};
// The image the scene was drawn with: index in `used`, or -1. `previous` is
// the last pick (0: none).
int choose(const std::vector<Usage>& used, VkExtent2D target, uint64_t previous);

// The layout to read a depth image in from `current`, and whether a
// transition is needed: false when it cannot be read from there.
bool readLayout(VkImageLayout current, VkImageLayout& read);

} // namespace depth

class DepthTracker {
public:
    struct Pick {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;  // ours: the depth aspect, mip 0, layer 0
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
        VkRect2D area{};                    // the part the game drew into this frame
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;  // after the frame's submissions
        bool reversed = false;              // cleared to 0 (near = 1)
        uint32_t passes = 0;
    };

    explicit DepthTracker(DeviceData& dev) : dev_(dev) {}
    ~DepthTracker();

    // vkCreateImage: the usage to create a depth image with (SAMPLED added,
    // when the format allows it, so that the layer can read it).
    VkImageUsageFlags usageFor(const VkImageCreateInfo& ci) const;
    void imageCreated(VkImage image, const VkImageCreateInfo& ci);
    // vkDestroyImage, before the driver's: our view of it goes (after the
    // queue that last read it is idle).
    void imageDestroying(VkImage image);
    void viewCreated(VkImageView view, const VkImageViewCreateInfo& ci);
    void viewDestroyed(VkImageView view);
    void framebufferCreated(VkFramebuffer fb, const VkFramebufferCreateInfo& ci);
    void framebufferDestroyed(VkFramebuffer fb);
    void renderPassCreated(VkRenderPass rp, const VkRenderPassCreateInfo& ci);
    void renderPassCreated2(VkRenderPass rp, const VkRenderPassCreateInfo2& ci);
    void renderPassDestroyed(VkRenderPass rp);
    void queueRetrieved(VkQueue queue, uint32_t family);
    bool queueFamily(VkQueue queue, uint32_t& family) const;

    void beginCommandBuffer(VkCommandBuffer cb);
    void beginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo& bi);
    void beginRendering(VkCommandBuffer cb, const VkRenderingInfo& ri);
    void barriers(VkCommandBuffer cb, uint32_t count, const VkImageMemoryBarrier* b);
    void barriers2(VkCommandBuffer cb, const VkDependencyInfo& di);
    void clearImage(VkCommandBuffer cb, VkImage image, float depth);
    void clearAttachments(VkCommandBuffer cb, uint32_t count, const VkClearAttachment* a);
    // The game's submission (its command buffers in order).
    void submitted(uint32_t count, const VkCommandBuffer* cbs);

    // At a present: the image this frame's scene was drawn with and its
    // state now; false (and why, in `why`) when none can be read. Resets
    // the frame's counters.
    bool pick(VkExtent2D target, Pick& out, std::string& why);
    // The queue our last read of `image` was submitted on (imageDestroying waits for it).
    void readOn(VkImage image, VkQueue queue);

private:
    struct Image {
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
        bool sampleable = false;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        float clear = std::numeric_limits<float>::quiet_NaN();  // last depth clear value
        uint32_t passes = 0;  // this frame
        VkRect2D area{};      // this frame
        VkImageView view = VK_NULL_HANDLE;  // ours, once picked
        VkQueue readQueue = VK_NULL_HANDLE;
    };
    struct Event {
        enum Kind : uint8_t { Pass, Layout, Clear } kind;
        uint64_t image;
        VkImageLayout layout;  // Pass: after the pass; Layout: the barrier's new layout
        VkRect2D area;
        float clear;           // NaN: none
    };
    struct RenderPass {
        int depth = -1;  // attachment index of the depth/stencil attachment
        VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        VkImageLayout finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    };
    struct Cb {
        std::vector<Event> events;
        uint64_t lastDepth = 0;  // the depth attachment of its current pass (vkCmdClearAttachments)
    };
    // Lock-free test for the barrier hook (every barrier of the game): is
    // this one of the depth images? (slots of their handles; 0 = free)
    static constexpr int kFast = 64;
    std::atomic<uint64_t> fast_[kFast] = {};
    std::atomic<bool> fastFull_{false};
    bool isTracked(uint64_t image) const;
    void addFast(uint64_t image);
    void removeFast(uint64_t image);
    Event* push(VkCommandBuffer cb, const Event& e);  // (mutex_ held)
    void passEvent(VkCommandBuffer cb, uint64_t image, VkImageLayout after, const VkRect2D& area, float clear);

    DeviceData& dev_;
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, Image> images_;
    std::unordered_map<uint64_t, uint64_t> views_;               // view -> image (depth images only)
    std::unordered_map<uint64_t, std::vector<uint64_t>> framebuffers_;  // attachments' images (0: not depth)
    std::unordered_map<uint64_t, RenderPass> renderPasses_;
    std::unordered_map<VkCommandBuffer, Cb> cbs_;
    std::unordered_map<VkQueue, uint32_t> queues_;
    uint64_t picked_ = 0;
};

} // namespace bdex
