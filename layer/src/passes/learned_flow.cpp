// learned_flow: the network of tools/train_flow.py corrects the finest flow
// (shaders/learned_flow.comp), after the passes that rewrite it
// (flow_temporal, subpixel_lk) and before those that read it.
#include "fg_pass.h"
#include "framegen_internal.h"
#include "shaders.h"

namespace bdex {
namespace {

struct LfPC { int32_t size[2]; int32_t luma[2]; int32_t block; int32_t stage; float strength; };

class LearnedFlowPass : public FgPass {
public:
    const char* name() const override { return "learned_flow"; }
    bool active(const Config& cfg) const override { return cfg.learnedFlow > 0.f; }

    void create(FgContext& ctx) override {
        const VkDescriptorType SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pipe_ = ctx.makePipeline(learned_flow_comp, learned_flow_comp_size, {SI, SI, SI, CIS, CIS});
        const FgFineLevel l0 = ctx.fineLevel();
        delta_ = ctx.createImage(VK_FORMAT_R32G32B32A32_SFLOAT, l0.fine, VK_IMAGE_USAGE_STORAGE_BIT);
        for (uint32_t parity = 0; parity < 2; ++parity) {
            VkDescriptorSet& s = sets_[parity];
            s = ctx.allocSet(pipe_.setLayout);
            ctx.writeImage(s, 0, SI, l0.flow[0]);
            ctx.writeImage(s, 1, SI, l0.flow[1]);
            ctx.writeImage(s, 2, SI, delta_.view);
            ctx.writeImage(s, 3, CIS, l0.pyr[1 - parity]);
            ctx.writeImage(s, 4, CIS, l0.pyr[parity]);
        }
    }
    void destroy(FgContext& ctx) override {
        ctx.destroyPipeline(pipe_);
        if (delta_.image) ctx.device().destroyImage(delta_);
    }

    void fineFlow(FgContext& ctx, VkCommandBuffer cmd, uint32_t parity, const Config& cfg) override {
        if (!(cfg.learnedFlow > 0.f) || !ctx.flowLastFrame()) return;
        auto& vt = ctx.device().vt;
        const FgFineLevel l0 = ctx.fineLevel();
        vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.pipeline);
        vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.layout, 0, 1, &sets_[parity], 0, nullptr);
        for (int32_t stage = 0; stage < 2; ++stage) {
            LfPC pc{{(int32_t)l0.fine.width, (int32_t)l0.fine.height}, {(int32_t)l0.luma.width, (int32_t)l0.luma.height},
                    (int32_t)l0.block, stage, std::min(1.f, cfg.learnedFlow)};
            vt.CmdPushConstants(cmd, pipe_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            vt.CmdDispatch(cmd, fg::divUp(l0.fine.width, 16), fg::divUp(l0.fine.height, 16), 1);
            ctx.barrier(cmd);
        }
    }

private:
    FgPipeline pipe_;
    AllocatedImage delta_;
    VkDescriptorSet sets_[2] = {};
};

} // namespace

std::unique_ptr<FgPass> makeLearnedFlowPass() { return std::make_unique<LearnedFlowPass>(); }

} // namespace bdex
