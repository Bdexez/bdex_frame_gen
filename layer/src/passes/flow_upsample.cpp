// flow_upsample: the finest flow brought to the luma resolution along the
// picture's edges (shaders/flow_upsample.comp), once per frame before its
// first synthesis — after every pass that rewrites the flow — and given to
// every synthesis in place of the block flow (FgContext::synthesisSetWithFlow).
#include "fg_pass.h"
#include "framegen_internal.h"
#include "shaders.h"

namespace bdex {
namespace {

struct UpPC { int32_t outSize[2]; int32_t fine[2]; int32_t luma[2]; int32_t block; float sigmaR; };

class FlowUpsamplePass : public FgPass {
public:
    const char* name() const override { return "flow_upsample"; }
    bool active(const Config& cfg) const override { return cfg.flowUpsample > 0.f; }

    void create(FgContext& ctx) override {
        const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        pipe_ = ctx.makePipeline(flow_upsample_comp, flow_upsample_comp_size, {CIS, CIS, CIS, SI});
        const FgFineLevel l0 = ctx.fineLevel();
        block_ = l0.block;
        out_ = {l0.fine.width * block_, l0.fine.height * block_};
        // (a pyramid level of about one texel per block: its luma is the blocks' mean)
        int lv = 0;
        for (uint32_t b = block_; b > 1 && lv + 1 < ctx.pyramidLevels(); b /= 2) ++lv;
        for (int d = 0; d < 2; ++d)
            up_[d] = ctx.createImage(VK_FORMAT_R32G32_SFLOAT, out_, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        for (uint32_t parity = 0; parity < 2; ++parity)
            for (int d = 0; d < 2; ++d) {
                // forward flow at the previous frame's positions, backward at the current one's
                const uint32_t frame = d == 0 ? 1 - parity : parity;
                VkDescriptorSet s = ctx.allocSet(pipe_.setLayout);
                ctx.writeImage(s, 0, CIS, l0.flow[d]);
                ctx.writeImage(s, 1, CIS, ctx.pyramidView(0, frame));
                ctx.writeImage(s, 2, CIS, ctx.pyramidView(lv, frame));
                ctx.writeImage(s, 3, SI, up_[d].view);
                upSets_[parity][d] = s;
            }
        n_ = ctx.sourceCount();
        for (uint32_t o = 0; o < ctx.outputs(); ++o)
            for (uint32_t prev = 0; prev < n_; ++prev)
                for (uint32_t cur = 0; cur < n_; ++cur)
                    synthSets_.push_back(
                        ctx.synthesisSetWithFlow(ctx.sourceView(prev), ctx.sourceView(cur), up_[0].view, up_[1].view, o));
        fine_ = l0.fine;
        luma_ = l0.luma;
    }
    void destroy(FgContext& ctx) override {
        for (auto& i : up_)
            if (i.image) ctx.device().destroyImage(i);
        ctx.destroyPipeline(pipe_);
    }

    void frameBegin(FgContext&) override { ++frame_; }
    void analysisDone(FgContext&, VkCommandBuffer, uint32_t parity) override { parity_ = parity & 1; }

    VkDescriptorSet synthesisInputs(FgContext& ctx, VkCommandBuffer cmd, uint32_t prev, uint32_t cur, uint32_t output) override {
        const Config cfg = ctx.effectiveConfig();
        if (!(cfg.flowUpsample > 0.f) || output >= ctx.outputs()) return VK_NULL_HANDLE;
        if (done_ != frame_) {  // once per frame: the flow is final by the first synthesis
            done_ = frame_;
            auto& vt = ctx.device().vt;
            UpPC pc{{(int32_t)out_.width, (int32_t)out_.height}, {(int32_t)fine_.width, (int32_t)fine_.height},
                    {(int32_t)luma_.width, (int32_t)luma_.height}, (int32_t)block_, cfg.flowUpsample};
            vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.pipeline);
            vt.CmdPushConstants(cmd, pipe_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            for (int d = 0; d < 2; ++d) {
                vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.layout, 0, 1, &upSets_[parity_][d], 0,
                                         nullptr);
                vt.CmdDispatch(cmd, fg::divUp(out_.width, 16), fg::divUp(out_.height, 16), 1);
            }
            ctx.barrier(cmd);
        }
        return synthSets_[(size_t(output) * n_ + prev) * n_ + cur];
    }

private:
    FgPipeline pipe_;
    AllocatedImage up_[2];
    VkDescriptorSet upSets_[2][2] = {};  // [parity][direction]
    std::vector<VkDescriptorSet> synthSets_;  // [(output * sources + prev) * sources + cur]
    VkExtent2D out_{}, fine_{}, luma_{};
    uint32_t block_ = 4, n_ = 0, parity_ = 0;
    uint64_t frame_ = 0, done_ = ~0ull;
};

} // namespace

std::unique_ptr<FgPass> makeFlowUpsamplePass() { return std::make_unique<FlowUpsamplePass>(); }

} // namespace bdex
