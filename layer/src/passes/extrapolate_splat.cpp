// extrapolate_splat: forward-splats the current frame to its predicted
// position before an extrapolated synthesis (see shaders/splat.comp); the
// synthesis takes each pixel's source from the splat map where one landed
// and backward-warps only the uncovered areas.
#include "fg_pass.h"
#include "framegen_internal.h"
#include "shaders.h"

namespace bdex {
namespace {

struct SplatPC { int32_t size[2]; float s; float flowScale; float flowInvSize[2]; int32_t timing; float accel; };

class ExtrapolateSplatPass : public FgPass {
public:
    const char* name() const override { return "extrapolate_splat"; }
    bool active(const Config& cfg) const override { return (cfg.extrapolate || cfg.hybrid) && cfg.extrapolateSplat; }

    void create(FgContext& ctx) override {
        const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                               SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pipe_ = ctx.makePipeline(splat_comp, splat_comp_size, {CIS, CIS, SI, SB, CIS});
        map_ = ctx.createImage(VK_FORMAT_R32_UINT, ctx.displayExtent(),
                               VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        const FgFineLevel l0 = ctx.fineLevel();
        set_ = ctx.allocSet(pipe_.setLayout);
        ctx.writeImage(set_, 0, CIS, l0.flow[0]);
        ctx.writeImage(set_, 1, CIS, l0.flow[1]);
        ctx.writeImage(set_, 2, SI, map_.view);
        ctx.writeBuffer(set_, 3, ctx.stateBuffer());
        ctx.writeImage(set_, 4, CIS, ctx.previousBackwardFlow());
        ctx.setSplatMap(map_.view);
    }
    void destroy(FgContext& ctx) override {
        if (map_.image) ctx.device().destroyImage(map_);
        ctx.destroyPipeline(pipe_);
    }

    void beforeSynthesis(FgContext& ctx, VkCommandBuffer cmd, float t, int32_t timing, bool gated, const Config& cfg,
                         SynthesisInputs& in) override {
        if (!(t > 1.f) || !cfg.extrapolateSplat) return;
        auto& vt = ctx.device().vt;
        const FgFineLevel l0 = ctx.fineLevel();
        const VkExtent2D d = ctx.displayExtent(), r = ctx.renderExtent();
        const float sx = float(d.width) / r.width, sy = float(d.height) / r.height, fs = ctx.flowToRender();
        VkClearColorValue none{};
        none.uint32[0] = 0xFFFFFFFFu;
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vt.CmdClearColorImage(cmd, map_.image, VK_IMAGE_LAYOUT_GENERAL, &none, 1, &range);
        ctx.barrier(cmd);
        vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.pipeline);
        SplatPC pc{{(int32_t)d.width, (int32_t)d.height}, t - 1.f, fs * sx,
                   {1.f / (l0.fine.width * l0.block * fs * sx), 1.f / (l0.fine.height * l0.block * fs * sy)},
                   timing, in.accel};
        vt.CmdPushConstants(cmd, pipe_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.layout, 0, 1, &set_, 0, nullptr);
        ctx.dispatchSynthesisSize(cmd, gated);
        ctx.barrier(cmd);
        in.splat = true;
    }

private:
    FgPipeline pipe_;
    AllocatedImage map_;  // forward-splat targets (display size)
    VkDescriptorSet set_ = VK_NULL_HANDLE;
};

} // namespace

std::unique_ptr<FgPass> makeExtrapolateSplatPass() { return std::make_unique<ExtrapolateSplatPass>(); }

} // namespace bdex
