// The optional passes of the engine, in hook order (see fg_pass.h). A pass
// is created only when its option can take effect in this configuration;
// options that may be toggled live create theirs whenever generation runs.
#include "fg_pass.h"

namespace bdex {

std::unique_ptr<FgPass> makeFlowTemporalPass();
std::unique_ptr<FgPass> makeOnlineTunePass();
std::unique_ptr<FgPass> makeTemporalUpscalePass();
std::unique_ptr<FgPass> makeSubpixelLkPass();
std::unique_ptr<FgPass> makeHudMaskPass();
std::unique_ptr<FgPass> makeExtrapolateAccelPass();
std::unique_ptr<FgPass> makeExtrapolateSplatPass();
std::unique_ptr<FgPass> makeFpsHudPass();

std::vector<std::unique_ptr<FgPass>> createOptionalPasses(const Config& cfg, DeviceData& dev, uint32_t outputs,
                                                          bool upscaling, VkExtent2D display) {
    std::vector<std::unique_ptr<FgPass>> passes;
    // The fps HUD draws on the real frame too, generation or not.
    if (cfg.overlayMode > 0) passes.push_back(makeFpsHudPass());
    if (outputs == 0) return passes;  // no generation: no flow to work on
    // Self-evaluation first: it rates the previous flow before anything rewrites it.
    if (cfg.onlineTune) passes.push_back(makeOnlineTunePass());
    passes.push_back(makeFlowTemporalPass());  // flow_temporal can be switched on live
    passes.push_back(makeSubpixelLkPass());    // likewise; after flow_temporal (it keeps whole-pixel vectors only)
    // Extrapolation's trajectory, then the splat along it (it reads the
    // acceleration weight). accel can be switched on live.
    passes.push_back(makeExtrapolateAccelPass());
    // The splat key packs the source index in 23 bits (4K fits). Also made
    // when online_tune may try it.
    if (cfg.extrapolate && (cfg.extrapolateSplat || cfg.onlineTune) &&
        uint64_t(display.width) * display.height <= (1ull << 23))
        passes.push_back(makeExtrapolateSplatPass());
    // Before upscale_temporal: its synthesis sets are made with the mask bound.
    if (cfg.hudMask > 0) passes.push_back(makeHudMaskPass());
    // Needs a filterable rgba16f history.
    if (upscaling && cfg.upscaleTemporal > 0.f &&
        dev.formatSupports(VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
        passes.push_back(makeTemporalUpscalePass());
    return passes;
}

} // namespace bdex
