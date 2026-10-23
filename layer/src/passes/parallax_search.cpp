// parallax_search: thin near structures moving faster than the scenery
// behind them (a railing's posts in a sideways move), which the hierarchical
// search gives the background's vector — multiples of the finest flow tried
// where it explains its window poorly (see shaders/flow_parallax.comp).
// Opt-in: on a fast-moving 1440p game it costs ~1 ms of flow a game frame,
// on the generated frames' latency path.
#include "fg_pass.h"
#include "framegen_internal.h"
#include "shaders.h"

namespace bdex {
namespace {

struct ParallaxPC { int32_t size[2]; int32_t luma[2]; int32_t block; float minSad; float gain; int32_t mode; float agree; };

class ParallaxSearchPass : public FgPass {
public:
    const char* name() const override { return "parallax_search"; }
    bool active(const Config& cfg) const override { return cfg.parallaxSearch; }

    void create(FgContext& ctx) override {
        const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        pipe_ = ctx.makePipeline(flow_parallax_comp, flow_parallax_comp_size, {SI, SI, CIS, CIS, SI, SI});
        const FgFineLevel l0 = ctx.fineLevel();
        for (AllocatedImage* img : {&candFwd_, &candBwd_})
            *img = ctx.createImage(VK_FORMAT_R32G32_SFLOAT, l0.fine, VK_IMAGE_USAGE_STORAGE_BIT);
        for (uint32_t parity = 0; parity < 2; ++parity) {
            VkDescriptorSet& s = sets_[parity];
            s = ctx.allocSet(pipe_.setLayout);
            ctx.writeImage(s, 0, SI, l0.flow[0]);
            ctx.writeImage(s, 1, SI, l0.flow[1]);
            ctx.writeImage(s, 2, CIS, l0.pyr[1 - parity]);
            ctx.writeImage(s, 3, CIS, l0.pyr[parity]);
            ctx.writeImage(s, 4, SI, candFwd_.view);
            ctx.writeImage(s, 5, SI, candBwd_.view);
        }
    }
    void destroy(FgContext& ctx) override {
        for (AllocatedImage* img : {&candFwd_, &candBwd_})
            if (img->image) ctx.device().destroyImage(*img);
        ctx.destroyPipeline(pipe_);
    }

    void fineFlow(FgContext& ctx, VkCommandBuffer cmd, uint32_t parity, const Config& cfg) override {
        if (!cfg.parallaxSearch) return;
        auto& vt = ctx.device().vt;
        const FgFineLevel l0 = ctx.fineLevel();
        vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.pipeline);
        // Measured (15/10, fence scene x4): poorly explained from a mean
        // |difference| of 0.015 (0.03: a quarter of the gain, 0.05: half),
        // a candidate kept below 0.7x the current SAD (0.5 and 0.85 alike),
        // the two directions within 2 pixels (1 and 4 alike).
        const float minSad = 0.015f, gain = 0.7f, agree = 2.f;
        ParallaxPC pc{{(int32_t)l0.fine.width, (int32_t)l0.fine.height},
                      {(int32_t)l0.luma.width, (int32_t)l0.luma.height}, (int32_t)l0.block, minSad, gain, 0, agree};
        vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.layout, 0, 1, &sets_[parity], 0, nullptr);
        for (int mode = 0; mode < 2; ++mode) {
            pc.mode = mode;
            vt.CmdPushConstants(cmd, pipe_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            vt.CmdDispatch(cmd, fg::divUp(l0.fine.width, 16), fg::divUp(l0.fine.height, 16), 1);
            ctx.barrier(cmd);
        }
    }

private:
    FgPipeline pipe_;
    VkDescriptorSet sets_[2] = {};
    AllocatedImage candFwd_, candBwd_;  // mode 0's candidates (NaN where none)
};

} // namespace

std::unique_ptr<FgPass> makeParallaxSearchPass() { return std::make_unique<ParallaxSearchPass>(); }

} // namespace bdex
