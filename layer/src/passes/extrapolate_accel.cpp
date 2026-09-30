// extrapolate_accel: extrapolation along a curved trajectory, from how the
// motion changed since the previous pair (see interpolate.comp, accelAt).
// The pass keeps the previous pair's flow and hands the synthesis its
// weight when that flow is really the previous pair's.
#include "fg_pass.h"

namespace bdex {
namespace {

class ExtrapolateAccelPass : public FgPass {
public:
    const char* name() const override { return "extrapolate_accel"; }
    bool active(const Config& cfg) const override { return cfg.extrapolate && cfg.extrapolateAccel > 0.f; }

    bool needsPreviousFlow(const Config& cfg) const override { return active(cfg); }
    void flowBegin(FgContext&, VkCommandBuffer, uint32_t) override { weight_ = 0.f; }
    // The previous flow was kept this frame when the one before computed a
    // flow (on a repeat, dedup keeps the flow: the acceleration is zero).
    void fineFlow(FgContext& ctx, VkCommandBuffer, uint32_t, const Config& cfg) override {
        weight_ = active(cfg) && ctx.flowLastFrame() ? cfg.extrapolateAccel : 0.f;
    }
    void beforeSynthesis(FgContext&, VkCommandBuffer, float t, int32_t, bool, const Config&,
                         SynthesisInputs& in) override {
        if (t > 1.f) in.accel = weight_;
    }

private:
    float weight_ = 0.f;  // for this frame's syntheses (0 = constant velocity)
};

} // namespace

std::unique_ptr<FgPass> makeExtrapolateAccelPass() { return std::make_unique<ExtrapolateAccelPass>(); }

} // namespace bdex
