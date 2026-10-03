// extrapolate_splat: forward-splats the current frame to its predicted
// position before an extrapolated synthesis (see shaders/splat.comp); the
// synthesis takes each pixel's source from the splat map where one landed
// and backward-warps only the uncovered areas.
#include "fg_pass.h"
#include "framegen_internal.h"
#include "shaders.h"

namespace bdex {
namespace {

struct SplatPC { int32_t size[2]; float s; float flowScale; float flowInvSize[2]; int32_t timing; float accel; int32_t mode; float t; };

class ExtrapolateSplatPass : public FgPass {
public:
    const char* name() const override { return "extrapolate_splat"; }
    bool active(const Config& cfg) const override {
        return ((cfg.extrapolate || cfg.hybrid) && cfg.extrapolateSplat) || cfg.interpSplat;
    }

    void create(FgContext& ctx) override {
        const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pipe_ = ctx.makePipeline(splat_comp, splat_comp_size, {CIS, CIS, SB, SB, CIS});
        // Twice the display's width: extrapolation uses the left half;
        // interp_splat prev's projection there and cur's on the right.
        const VkExtent2D d = ctx.displayExtent();
        map_ = ctx.device().createBuffer(4 * VkDeviceSize(2 * d.width) * d.height,
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        const FgFineLevel l0 = ctx.fineLevel();
        set_ = ctx.allocSet(pipe_.setLayout);
        ctx.writeImage(set_, 0, CIS, l0.flow[0]);
        ctx.writeImage(set_, 1, CIS, l0.flow[1]);
        ctx.writeBuffer(set_, 2, map_.buffer);
        ctx.writeBuffer(set_, 3, ctx.stateBuffer());
        ctx.writeImage(set_, 4, CIS, ctx.previousBackwardFlow());
        ctx.setSplatMap(map_.buffer);
    }
    void destroy(FgContext& ctx) override {
        if (map_.buffer) ctx.device().destroyBuffer(map_);
        ctx.destroyPipeline(pipe_);
    }

    void beforeSynthesis(FgContext& ctx, VkCommandBuffer cmd, float t, int32_t timing, bool gated, const Config& cfg,
                         SynthesisInputs& in) override {
        const bool extrap = t > 1.f && cfg.extrapolateSplat, interp = t <= 1.f && cfg.interpSplat;
        if (!extrap && !interp) return;
        auto& vt = ctx.device().vt;
        const FgFineLevel l0 = ctx.fineLevel();
        const VkExtent2D d = ctx.displayExtent(), r = ctx.renderExtent();
        const float sx = float(d.width) / r.width, sy = float(d.height) / r.height, fs = ctx.flowToRender();
        // (no key: nothing landed) — interp_splat's half-resolution map is a quarter of it
        const uint32_t hw = (d.width + 1) / 2, hh = (d.height + 1) / 2;
        vt.CmdFillBuffer(cmd, map_.buffer, 0, extrap ? VK_WHOLE_SIZE : 4 * VkDeviceSize(2 * hw) * hh, 0xFFFFFFFFu);
        ctx.barrier(cmd);
        vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.pipeline);
        SplatPC pc{{(int32_t)d.width, (int32_t)d.height}, t - 1.f, fs * sx,
                   {1.f / (l0.fine.width * l0.block * fs * sx), 1.f / (l0.fine.height * l0.block * fs * sy)},
                   timing, extrap ? in.accel : 0.f, 0, t};
        vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.layout, 0, 1, &set_, 0, nullptr);
        pc.mode = extrap ? 0 : 1;
        vt.CmdPushConstants(cmd, pipe_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        if (extrap) ctx.dispatchSynthesisSize(cmd, gated);
        else vt.CmdDispatch(cmd, (hw + 15) / 16, (hh + 15) / 16, 1);  // (a thread per 2x2 block)
        ctx.barrier(cmd);
        in.splat = true;
    }

private:
    FgPipeline pipe_;
    AllocatedBuffer map_;  // forward-splat targets (2 x display size, see splat.comp)
    VkDescriptorSet set_ = VK_NULL_HANDLE;
};

} // namespace

std::unique_ptr<FgPass> makeExtrapolateSplatPass() { return std::make_unique<ExtrapolateSplatPass>(); }

} // namespace bdex
