// The optional passes of the engine, in hook order (see fg_pass.h). A pass
// is created only when its option can take effect in this configuration;
// options that may be toggled live create theirs whenever generation runs.
#include "fg_pass.h"

namespace bdex {

std::unique_ptr<FgPass> makeFlowTemporalPass();
std::unique_ptr<FgPass> makeOnlineTunePass();
std::unique_ptr<FgPass> makeTemporalUpscalePass();
std::unique_ptr<FgPass> makeSubpixelLkPass();

std::vector<std::unique_ptr<FgPass>> createOptionalPasses(const Config& cfg, DeviceData& dev, uint32_t outputs,
                                                          bool upscaling) {
    std::vector<std::unique_ptr<FgPass>> passes;
    if (outputs == 0) return passes;  // no generation: no flow to work on
    // Self-evaluation first: it rates the previous flow before anything rewrites it.
    if (cfg.onlineTune) passes.push_back(makeOnlineTunePass());
    passes.push_back(makeFlowTemporalPass());  // flow_temporal can be switched on live
    passes.push_back(makeSubpixelLkPass());    // likewise; after flow_temporal (it keeps whole-pixel vectors only)
    // Needs a filterable rgba16f history.
    if (upscaling && cfg.upscaleTemporal > 0.f &&
        dev.formatSupports(VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
        passes.push_back(makeTemporalUpscalePass());
    return passes;
}

} // namespace bdex
