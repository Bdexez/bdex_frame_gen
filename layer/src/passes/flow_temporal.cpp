// flow_temporal: temporal stabilisation of the finest flow (see
// shaders/flow_temporal.comp) — keeps the previous pair's vector where it
// explains the new pair about as well.
#include "fg_pass.h"
#include "framegen_internal.h"
#include "shaders.h"

namespace bdex {
namespace {

struct TemporalPC { int32_t size[2]; int32_t luma[2]; int32_t block; float strength; };

class FlowTemporalPass : public FgPass {
public:
    const char* name() const override { return "flow_temporal"; }

    void create(FgContext& ctx) override {
        const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        pipe_ = ctx.makePipeline(flow_temporal_comp, flow_temporal_comp_size, {SI, SI, CIS, CIS, CIS});
        const FgFineLevel l0 = ctx.fineLevel();
        for (uint32_t parity = 0; parity < 2; ++parity) {
            VkDescriptorSet& s = sets_[parity];
            s = ctx.allocSet(pipe_.setLayout);
            ctx.writeImage(s, 0, SI, l0.flow[0]);
            ctx.writeImage(s, 1, SI, l0.flow[1]);
            ctx.writeImage(s, 2, CIS, ctx.previousBackwardFlow());
            ctx.writeImage(s, 3, CIS, l0.pyr[1 - parity]);
            ctx.writeImage(s, 4, CIS, l0.pyr[parity]);
        }
    }
    void destroy(FgContext& ctx) override { ctx.destroyPipeline(pipe_); }

    bool needsPreviousFlow(const Config& cfg) const override { return cfg.flowTemporal > 0.f; }

    void fineFlow(FgContext& ctx, VkCommandBuffer cmd, uint32_t parity, const Config& cfg) override {
        // (On a repeat the flows are kept and the previous flow equals them: no-op.)
        if (!(cfg.flowTemporal > 0.f) || !ctx.flowLastFrame()) return;
        auto& vt = ctx.device().vt;
        const FgFineLevel l0 = ctx.fineLevel();
        vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.pipeline);
        TemporalPC pc{{(int32_t)l0.fine.width, (int32_t)l0.fine.height},
                      {(int32_t)l0.luma.width, (int32_t)l0.luma.height}, (int32_t)l0.block, cfg.flowTemporal};
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

std::unique_ptr<FgPass> makeFlowTemporalPass() { return std::make_unique<FlowTemporalPass>(); }

} // namespace bdex
