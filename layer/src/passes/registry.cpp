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
std::unique_ptr<FgPass> makeFlowConfidencePass();
std::unique_ptr<FgPass> makeLearnedCorrectionPass();
std::unique_ptr<FgPass> makeFalseDetailPass();
std::unique_ptr<FgPass> makeMosaicBlurPass();
std::unique_ptr<FgPass> makeHudLayerPass();
std::unique_ptr<FgPass> makeParallaxSearchPass();
std::unique_ptr<FgPass> makeLumaMatchPass();
std::unique_ptr<FgPass> makeLearnedFlowPass();

std::vector<std::unique_ptr<FgPass>> createOptionalPasses(const Config& cfg, DeviceData& dev, uint32_t outputs,
                                                          bool upscaling, VkExtent2D display) {
    std::vector<std::unique_ptr<FgPass>> passes;
    // The fps HUD draws on the real frame too, generation or not — last, over
    // what the other passes did to the finished image (learned_correction).
    auto withHud = [&]() -> std::vector<std::unique_ptr<FgPass>> {
        if (cfg.overlayMode > 0) passes.push_back(makeFpsHudPass());
        return std::move(passes);
    };
    if (outputs == 0) return withHud();  // no generation: no flow to work on
    // The previous pyramid at the current brightness, before anything matches on it.
    if (cfg.lumaMatch > 0.f) passes.push_back(makeLumaMatchPass());
    // Self-evaluation first: it rates the previous flow before anything rewrites it.
    if (cfg.onlineTune) passes.push_back(makeOnlineTunePass());
    passes.push_back(makeFlowTemporalPass());  // flow_temporal can be switched on live
    passes.push_back(makeParallaxSearchPass());  // whole-pixel vectors: before subpixel_lk
    passes.push_back(makeSubpixelLkPass());    // likewise; after flow_temporal (it keeps whole-pixel vectors only)
    if (cfg.learnedFlow > 0.f) passes.push_back(makeLearnedFlowPass());  // the finest flow as the others left it
    // Extrapolation's trajectory, then the splat along it (it reads the
    // acceleration weight). accel can be switched on live.
    passes.push_back(makeExtrapolateAccelPass());
    // The splat key packs the source index in 23 bits (4K fits). Also made
    // when online_tune may try it.
    // interp_splat: the same map, for the interpolated frames.
    if ((((cfg.extrapolate || cfg.hybrid) && (cfg.extrapolateSplat || cfg.onlineTune)) || cfg.interpSplat) &&
        uint64_t(display.width) * display.height <= (1ull << 23))
        passes.push_back(makeExtrapolateSplatPass());
    // Before upscale_temporal: its synthesis sets are made with the mask bound.
    if (cfg.hudMask > 0) passes.push_back(makeHudMaskPass());
    // Its synthesis sets replace the engine's (upscale_temporal's, made
    // later, would replace them in turn: the two do not combine).
    // Likewise with the confidence map (after the passes that rewrite the
    // finest flow: flow_temporal, subpixel_lk).
    if (cfg.flowConfidence || cfg.debug == Config::Debug::Confidence)
        passes.push_back(makeFlowConfidencePass());
    // Needs a filterable rgba16f history.
    if (upscaling && cfg.upscaleTemporal > 0.f &&
        dev.formatSupports(VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
        passes.push_back(makeTemporalUpscalePass());
    // Over the finished generated frames (8-bit, at render size; it checks).
    // The generated frame's own false detail (before learned_correction and
    // the HUD, which read the finished output). Can be switched on live.
    passes.push_back(makeFalseDetailPass());
    passes.push_back(makeMosaicBlurPass());  // (after false_detail: measured over its output)
    passes.push_back(makeHudLayerPass());    // (after mosaic_blur: measured over its output)
    if (cfg.learnedCorrection > 0.f) passes.push_back(makeLearnedCorrectionPass());
    return withHud();
}

} // namespace bdex
