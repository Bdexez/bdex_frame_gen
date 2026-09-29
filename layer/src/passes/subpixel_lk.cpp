// subpixel_lk: sub-pixel refinement of the finest flow by Lucas-Kanade (see
// shaders/flow_lk.comp), after the whole-pixel block matching.
#include "fg_pass.h"
#include "framegen_internal.h"
#include "shaders.h"

namespace bdex {
namespace {

struct LkPC { int32_t size[2]; int32_t luma[2]; int32_t block; int32_t iterations; float minEigen; };

class SubpixelLkPass : public FgPass {
public:
    const char* name() const override { return "subpixel_lk"; }

    void create(FgContext& ctx) override {
        const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        pipe_ = ctx.makePipeline(flow_lk_comp, flow_lk_comp_size, {SI, SI, CIS, CIS});
        const FgFineLevel l0 = ctx.fineLevel();
        for (uint32_t parity = 0; parity < 2; ++parity) {
            VkDescriptorSet& s = sets_[parity];
            s = ctx.allocSet(pipe_.setLayout);
            ctx.writeImage(s, 0, SI, l0.flow[0]);
            ctx.writeImage(s, 1, SI, l0.flow[1]);
            ctx.writeImage(s, 2, CIS, l0.pyr[1 - parity]);
            ctx.writeImage(s, 3, CIS, l0.pyr[parity]);
        }
    }
    void destroy(FgContext& ctx) override { ctx.destroyPipeline(pipe_); }

    void fineFlow(FgContext& ctx, VkCommandBuffer cmd, uint32_t parity, const Config& cfg) override {
        if (cfg.subpixelLk <= 0) return;
        auto& vt = ctx.device().vt;
        const FgFineLevel l0 = ctx.fineLevel();
        // A gradient of about 1/255 per pixel in both directions: below it the
        // window is too flat (or 1-D) for a sub-pixel estimate.
        const float minEigen = 1.f / (255.f * 255.f);
        LkPC pc{{(int32_t)l0.fine.width, (int32_t)l0.fine.height}, {(int32_t)l0.luma.width, (int32_t)l0.luma.height},
                (int32_t)l0.block, cfg.subpixelLk, minEigen};
        vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.pipeline);
        vt.CmdPushConstants(cmd, pipe_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.layout, 0, 1, &sets_[parity], 0, nullptr);
        vt.CmdDispatch(cmd, fg::divUp(l0.fine.width, 16), fg::divUp(l0.fine.height, 16), 1);
        ctx.barrier(cmd);
    }

private:
    FgPipeline pipe_;
    VkDescriptorSet sets_[2] = {};
};

} // namespace

std::unique_ptr<FgPass> makeSubpixelLkPass() { return std::make_unique<SubpixelLkPass>(); }

} // namespace bdex
