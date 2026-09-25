#pragma once
// Optional passes of the frame-generation engine.
//
// FrameGen runs the fixed pipeline (analysis, hierarchical flow, synthesis,
// output). An optional pass — a feature that is off by default and adds GPU
// work at a precise point — is a small FgPass object instead of code spread
// over FrameGen's creation, descriptor, recording and destruction functions:
// it creates what it needs in create(), releases it in destroy(), and
// records at the hooks below. It sees FrameGen only through FgContext, the
// narrow set of engine resources passes use.
//
// Adding one: a class in layer/src/passes/, one line in createOptionalPasses
// (passes/registry.cpp), its source in BDEX_ENGINE_SOURCES
// (layer/CMakeLists.txt, shared by the layer, bdex_offline and bdex_capture),
// its option in the registry of config.cpp.
#include "config.h"
#include "device.h"
#include "format.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace bdex {

// What the passes contribute to a synthesis (beforeSynthesis).
struct SynthesisInputs {
    bool splat = false;  // the splat map holds this synthesis's forward splat
    float accel = 0.f;   // extrapolation: weight of the acceleration term
};

// The fps HUD's figures (FrameGen::setHud).
struct FgHud {
    int mode = 0, game = 0, out = 0, low1 = 0, low01 = 0, latency = -1;
};

// A compute pipeline with its one descriptor set layout.
struct FgPipeline {
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

// The finest flow level (level 0 of the pyramid).
struct FgFineLevel {
    VkExtent2D luma{};        // luma resolution (flow vectors are in these pixels)
    VkExtent2D fine{};        // flow texels
    uint32_t block = 4;       // luma pixels per flow texel
    VkImageView pyr[2] = {};  // luma per parity: [parity] = current frame, [1 - parity] = previous
    VkImageView flow[2] = {}; // [0] forward (at previous-frame positions), [1] backward (at current ones)
    VkImage flowImage[2] = {};  // the same images (TRANSFER_SRC: they may be copied)
};

// What a pass may use of the engine.
class FgContext {
public:
    virtual ~FgContext() = default;
    virtual DeviceData& device() = 0;
    virtual const Config& baseConfig() const = 0;  // the settings at hand, without overrides
    virtual Config effectiveConfig() const = 0;     // ... with this frame's overrides (passes, GPU budget)

    // Resources.
    virtual FgPipeline makePipeline(const uint32_t* spv, size_t spvSize, const std::vector<VkDescriptorType>& bindings) = 0;
    virtual void destroyPipeline(FgPipeline& p) = 0;
    virtual VkDescriptorSet allocSet(VkDescriptorSetLayout layout) = 0;
    virtual void writeImage(VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkImageView view) = 0;
    virtual void writeBuffer(VkDescriptorSet set, uint32_t binding, VkBuffer buffer) = 0;
    // A new internal image, in GENERAL layout for its whole life (as FrameGen's own).
    virtual AllocatedImage createImage(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage) = 0;
    // Compute/transfer writes before compute/transfer/indirect reads and writes.
    virtual void barrier(VkCommandBuffer cmd) = 0;

    // The pipeline's images and buffers.
    virtual FgFineLevel fineLevel() const = 0;
    virtual VkImageView previousBackwardFlow() const = 0;  // the previous pair's finest backward flow (see needsPreviousFlow)
    virtual VkBuffer stateBuffer() const = 0;              // frame state (frame_state.comp layout)
    virtual uint32_t sourceCount() const = 0;
    virtual VkFormat sourceFormat() const = 0;
    virtual VkImage sourceImage(uint32_t index) const = 0;  // TRANSFER_SRC; GENERAL between acquire and release
    virtual VkImageView sourceView(uint32_t index) const = 0;
    virtual VkExtent2D renderExtent() const = 0;
    virtual VkExtent2D displayExtent() const = 0;
    virtual uint32_t outputs() const = 0;                 // generated frames per real frame
    virtual float flowToRender() const = 0;               // flow units -> render pixels
    virtual bool upscaling() const = 0;

    // Per frame.
    virtual bool flowLastFrame() const = 0;    // the flow images hold the flow of the previous frame's pair
    virtual bool repeatFlagValid() const = 0;  // the state buffer's repeat flag (dedup) is this frame's

    // Descriptor sets of the engine's own passes with other inputs: the
    // synthesis of output `output` from `prev` / `cur`, and the real frame's
    // output stage (packing, HUD) from `src`.
    virtual VkDescriptorSet synthesisSet(VkImageView prev, VkImageView cur, uint32_t output) = 0;
    virtual VkDescriptorSet realOutputSet(VkImageView src) = 0;
    // A per-render-pixel count of frames a pixel stayed unchanged (r32f):
    // every synthesis keeps pixels at or above hud_mask's threshold as they
    // are (a 1x1 zero image until a pass provides one).
    virtual void setStaticMask(VkImageView view) = 0;
    // The forward-splat map (r32ui, display size) every synthesis reads when
    // SynthesisInputs::splat is set (a 1x1 image until a pass provides one).
    virtual void setSplatMap(VkImageView view) = 0;
    // A dispatch over the display in 16x16 groups — or, `gated`, through the
    // frame state's "only on repeated frames" entry (dedup's replacement).
    virtual void dispatchSynthesisSize(VkCommandBuffer cmd, bool gated) = 0;

    // The finished output images (swapchain storage encoding): generated
    // output `output`, or the real frame's for -1 (when it is packed).
    virtual VkImageView outputView(int output) const = 0;
    virtual OutputEncoding outputEncoding() const = 0;
    virtual FgHud hud() const = 0;

    // Syntheses of a pass's own (online_tune's probe): the synthesis shader
    // writing linear rgba16f whatever the swapchain format, its descriptor
    // sets with any inputs, flows and output, and a synthesis recorded with
    // given settings (no fps HUD, fixed t).
    virtual FgPipeline makeLinearSynthesisPipeline() = 0;
    virtual VkDescriptorSet synthesisSetFor(VkDescriptorSetLayout layout, VkImageView prev, VkImageView cur,
                                            VkImageView fwd, VkImageView bwd, VkImageView out) = 0;
    virtual void recordSynthesisWith(VkCommandBuffer cmd, const FgPipeline& pipe, VkDescriptorSet set, float t,
                                     const Config& cfg) = 0;
};

class FgPass {
public:
    virtual ~FgPass() = default;
    virtual const char* name() const = 0;
    // Doing work with these settings (a pass kept for a live toggle may idle).
    virtual bool active(const Config&) const { return true; }

    // After the engine's own resources exist.
    virtual void create(FgContext&) {}
    virtual void destroy(FgContext&) {}

    // Hooks, in pipeline order.
    virtual void frameBegin(FgContext&) {}                      // every recorded frame (recordAcquireSources)
    virtual void slotBegin(FgContext&, uint32_t /*slot*/) {}    // a command slot is reused: its previous submission completed
    virtual void analysisDone(FgContext&, VkCommandBuffer, uint32_t /*parity*/) {}  // the new luma pyramid exists
    // recordFlow is starting: the flow images still hold the previous pair's.
    virtual void flowBegin(FgContext&, VkCommandBuffer, uint32_t /*parity*/) {}
    // Settings for this frame's flow and syntheses (called on a copy).
    virtual void overrideConfig(Config&) const {}
    // The previous pair's backward flow must be kept (previousBackwardFlow).
    virtual bool needsPreviousFlow(const Config&) const { return false; }
    virtual void fineFlow(FgContext&, VkCommandBuffer, uint32_t /*parity*/, const Config&) {}  // finest flow done, before the scene-cut reduction
    // Before a synthesis: a descriptor set to use instead of the engine's
    // (VK_NULL_HANDLE keeps it).
    virtual VkDescriptorSet synthesisInputs(FgContext&, VkCommandBuffer, uint32_t /*prev*/, uint32_t /*cur*/,
                                            uint32_t /*output*/) {
        return VK_NULL_HANDLE;
    }
    // The real frame's output stage: replace its descriptor set and source
    // extent (return false to keep the spatial upscale of the source).
    virtual bool realOutput(FgContext&, VkCommandBuffer, uint32_t /*cur*/, VkDescriptorSet& /*set*/, VkExtent2D& /*src*/) {
        return false;
    }
    // An output image is finished (upscaled, sharpened): generated output
    // `output`, or -1 for the real frame. Overlays go here (the fps HUD).
    virtual void afterOutput(FgContext&, VkCommandBuffer, int /*output*/) {}
    // Just before each synthesis at time t (extrapolation: t > 1; `timing`
    // and `gated` as the synthesis's): record work it needs and set its inputs.
    virtual void beforeSynthesis(FgContext&, VkCommandBuffer, float /*t*/, int32_t /*timing*/, bool /*gated*/,
                                 const Config&, SynthesisInputs&) {}
};

// The optional passes this configuration enables, in hook order
// (passes/registry.cpp). `outputs`: generated frames per real frame;
// `display`: the presented size.
std::vector<std::unique_ptr<FgPass>> createOptionalPasses(const Config& cfg, DeviceData& dev, uint32_t outputs,
                                                          bool upscaling, VkExtent2D display);

} // namespace bdex
