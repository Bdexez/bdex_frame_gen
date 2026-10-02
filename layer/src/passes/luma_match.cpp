// luma_match: the previous frame's luma pyramid brought to the current
// frame's brightness (shaders/luma_match.comp) as soon as the new pyramid
// exists, before the flow — fades, flashes, lighting changes match as the
// same picture. Only the matching sees it: the synthesis samples the frames
// themselves.
#include "fg_pass.h"
#include "framegen_internal.h"
#include "shaders.h"

namespace bdex {
namespace {

struct LumaMatchPC { int32_t size[2]; int32_t mode; float minShift; };

class LumaMatchPass : public FgPass {
public:
    const char* name() const override { return "luma_match"; }
    bool active(const Config& cfg) const override { return cfg.lumaMatch > 0.f; }

    void create(FgContext& ctx) override {
        const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                               SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        const bool half = ctx.lumaFormat() == VK_FORMAT_R16_SFLOAT;
        pipe_ = half ? ctx.makePipeline(luma_match_comp_f16, luma_match_comp_f16_size, {CIS, CIS, SI, SB})
                     : ctx.makePipeline(luma_match_comp_f32, luma_match_comp_f32_size, {CIS, CIS, SI, SB});
        stats_ = ctx.device().createBuffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        levels_ = ctx.pyramidLevels();
        const int top = levels_ - 1;
        for (uint32_t parity = 0; parity < 2; ++parity)
            for (int l = 0; l < levels_; ++l) {
                VkDescriptorSet s = ctx.allocSet(pipe_.setLayout);
                ctx.writeImage(s, 0, CIS, ctx.pyramidView(top, parity));
                ctx.writeImage(s, 1, CIS, ctx.pyramidView(top, 1 - parity));
                ctx.writeImage(s, 2, SI, ctx.pyramidView(l, 1 - parity));
                ctx.writeBuffer(s, 3, stats_.buffer);
                sets_[parity].push_back(s);
            }
    }
    void destroy(FgContext& ctx) override {
        ctx.device().destroyBuffer(stats_);
        ctx.destroyPipeline(pipe_);
    }

    void analysisDone(FgContext& ctx, VkCommandBuffer cmd, uint32_t parity) override {
        const Config cfg = ctx.effectiveConfig();
        if (!(cfg.lumaMatch > 0.f)) return;
        auto& vt = ctx.device().vt;
        const auto& sets = sets_[parity & 1];
        vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.pipeline);
        const VkExtent2D top = ctx.pyramidExtent(levels_ - 1);
        LumaMatchPC pc{{(int32_t)top.width, (int32_t)top.height}, 0, cfg.lumaMatch};
        vt.CmdPushConstants(cmd, pipe_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.layout, 0, 1, &sets[0], 0, nullptr);
        vt.CmdDispatch(cmd, 1, 1, 1);
        ctx.barrier(cmd);
        for (int l = 0; l < levels_; ++l) {
            // (every dispatch binds the coarsest level as read; the last one
            // writes it: after the others' reads)
            if (l == levels_ - 1 && l > 0) ctx.barrier(cmd);
            const VkExtent2D e = ctx.pyramidExtent(l);
            pc = {{(int32_t)e.width, (int32_t)e.height}, 1, cfg.lumaMatch};
            vt.CmdPushConstants(cmd, pipe_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.layout, 0, 1, &sets[size_t(l)], 0, nullptr);
            vt.CmdDispatch(cmd, fg::divUp(e.width, 16), fg::divUp(e.height, 16), 1);
        }
        ctx.barrier(cmd);
    }

private:
    FgPipeline pipe_;
    AllocatedBuffer stats_;
    int levels_ = 0;
    std::vector<VkDescriptorSet> sets_[2];  // [parity][level]
};

} // namespace

std::unique_ptr<FgPass> makeLumaMatchPass() { return std::make_unique<LumaMatchPass>(); }

} // namespace bdex
