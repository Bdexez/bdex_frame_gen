// extrapolate_accel / interp_accel: extrapolated and interpolated frames
// along a curved trajectory, from how the motion changed since the previous
// pair (see interpolate.comp, accelAt). The pass keeps the previous pair's
// flow and hands the synthesis its weight when that flow is really the
// previous pair's.
#include "fg_pass.h"
#include "framegen_internal.h"
#include "shaders.h"

namespace bdex {
namespace {

struct AccelMapPC { int32_t fine[2]; int32_t size[2]; float flowInvSize[2]; float flowScale; };

class ExtrapolateAccelPass : public FgPass {
public:
    const char* name() const override { return "extrapolate_accel"; }
    bool active(const Config& cfg) const override {
        return ((cfg.extrapolate || cfg.hybrid) && cfg.extrapolateAccel > 0.f) || ((!cfg.extrapolate || cfg.hybrid) && cfg.interpAccel > 0.f);
    }

    // interp_accel's map (accel_map.comp): finest flow size, once per frame.
    void create(FgContext& ctx) override {
        const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        pipe_ = ctx.makePipeline(accel_map_comp, accel_map_comp_size, {CIS, CIS, CIS, SI});
        const FgFineLevel l0 = ctx.fineLevel();
        map_ = ctx.createImage(VK_FORMAT_R32G32_SFLOAT, l0.fine, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        set_ = ctx.allocSet(pipe_.setLayout);
        ctx.writeImage(set_, 0, CIS, l0.flow[0]);
        ctx.writeImage(set_, 1, CIS, l0.flow[1]);
        ctx.writeImage(set_, 2, CIS, ctx.previousBackwardFlow());
        ctx.writeImage(set_, 3, SI, map_.view);
        ctx.setAccelMap(map_.view);
    }
    void destroy(FgContext& ctx) override {
        if (map_.image) ctx.device().destroyImage(map_);
        ctx.destroyPipeline(pipe_);
    }

    bool needsPreviousFlow(const Config& cfg) const override { return active(cfg); }
    void flowBegin(FgContext&, VkCommandBuffer, uint32_t) override { weight_ = interpWeight_ = 0.f; }
    // The previous flow was kept this frame when the one before computed a
    // flow (on a repeat, dedup keeps the flow: the acceleration is zero).
    void fineFlow(FgContext& ctx, VkCommandBuffer cmd, uint32_t, const Config& cfg) override {
        const bool kept = active(cfg) && ctx.flowLastFrame();
        weight_ = kept && (cfg.extrapolate || cfg.hybrid) ? cfg.extrapolateAccel : 0.f;
        interpWeight_ = kept && (!cfg.extrapolate || cfg.hybrid) ? cfg.interpAccel : 0.f;
        if (interpWeight_ <= 0.f) return;
        const FgFineLevel l0 = ctx.fineLevel();
        const VkExtent2D r = ctx.renderExtent(), d = ctx.displayExtent();
        const float sx = float(d.width) / r.width, sy = float(d.height) / r.height, f2r = ctx.flowToRender();
        AccelMapPC pc{{(int32_t)l0.fine.width, (int32_t)l0.fine.height}, {(int32_t)d.width, (int32_t)d.height},
                      {1.f / (l0.fine.width * l0.block * f2r * sx), 1.f / (l0.fine.height * l0.block * f2r * sy)}, f2r * sx};
        auto& vt = ctx.device().vt;
        vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.pipeline);
        vt.CmdPushConstants(cmd, pipe_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.layout, 0, 1, &set_, 0, nullptr);
        vt.CmdDispatch(cmd, fg::divUp(l0.fine.width, 16), fg::divUp(l0.fine.height, 16), 1);
        ctx.barrier(cmd);
    }
    void beforeSynthesis(FgContext&, VkCommandBuffer, float t, int32_t, bool, const Config&,
                         SynthesisInputs& in) override {
        in.accel = t > 1.f ? weight_ : interpWeight_;
    }

private:
    float weight_ = 0.f;        // for this frame's extrapolations (0 = constant velocity)
    float interpWeight_ = 0.f;  // ... and interpolations
    FgPipeline pipe_;
    AllocatedImage map_;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
};

} // namespace

std::unique_ptr<FgPass> makeExtrapolateAccelPass() { return std::make_unique<ExtrapolateAccelPass>(); }

} // namespace bdex
