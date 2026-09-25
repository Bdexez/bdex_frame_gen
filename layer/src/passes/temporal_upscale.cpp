// upscale_temporal: flow-guided temporal upscaling for render_scale < 1 (see
// shaders/upscale_temporal.comp). The real frame is upscaled into a
// display-size history (ping-pong) blended with the previous one carried
// along the flow; the real frame's output is packed from it, and the
// syntheses read the two histories instead of the small source images.
#include "fg_pass.h"
#include "framegen_internal.h"
#include "shaders.h"

#include <algorithm>

namespace bdex {
namespace {

struct TaaPC { int32_t outSize[2]; int32_t srcSize[2]; float flowInvSize[2]; float flowScale; float weight; float cutLow; float cutHigh; int32_t filterMode; int32_t hasHistory; int32_t useDup; };

class TemporalUpscalePass : public FgPass {
public:
    const char* name() const override { return "upscale_temporal"; }

    void create(FgContext& ctx) override {
        const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                               SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pipe_ = ctx.makePipeline(upscale_temporal_comp, upscale_temporal_comp_size, {CIS, CIS, CIS, SI, SB, CIS});
        for (auto& h : hist_)
            h = ctx.createImage(VK_FORMAT_R16G16B16A16_SFLOAT, ctx.displayExtent(),
                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        const FgFineLevel l0 = ctx.fineLevel();
        for (uint32_t n = 0; n < ctx.sourceCount(); ++n)
            for (uint32_t h = 0; h < 2; ++h) {
                VkDescriptorSet s = ctx.allocSet(pipe_.setLayout);
                ctx.writeImage(s, 0, CIS, ctx.sourceView(n));
                ctx.writeImage(s, 1, CIS, hist_[1 - h].view);
                ctx.writeImage(s, 2, CIS, l0.flow[1]);
                ctx.writeImage(s, 3, SI, hist_[h].view);
                ctx.writeBuffer(s, 4, ctx.stateBuffer());
                ctx.writeImage(s, 5, CIS, l0.flow[0]);
                accum_.push_back(s);
            }
        for (uint32_t h = 0; h < 2; ++h) {
            output_[h] = ctx.realOutputSet(hist_[h].view);
            for (uint32_t o = 0; o < ctx.outputs(); ++o) synth_.push_back(ctx.synthesisSet(hist_[1 - h].view, hist_[h].view, o));
        }
    }
    void destroy(FgContext& ctx) override {
        for (auto& h : hist_)
            if (h.image) ctx.device().destroyImage(h);
        ctx.destroyPipeline(pipe_);
    }

    void frameBegin(FgContext&) override { ++frame_; }
    void flowBegin(FgContext&, VkCommandBuffer, uint32_t) override { flowFrame_ = frame_; }

    VkDescriptorSet synthesisInputs(FgContext& ctx, VkCommandBuffer cmd, uint32_t cur, uint32_t output) override {
        // Synthesise from the accumulated history of both frames, so the
        // generated frames match the real ones (else they alternate between
        // accumulated and plain upscale).
        record(ctx, cmd, cur);
        return prevValid_ ? synth_[h_ * ctx.outputs() + output] : VK_NULL_HANDLE;
    }

    bool realOutput(FgContext& ctx, VkCommandBuffer cmd, uint32_t cur, VkDescriptorSet& set, VkExtent2D& src) override {
        // The real frame is the accumulated history, only packed (and given
        // the HUD) at 1:1.
        record(ctx, cmd, cur);
        set = output_[h_];
        src = ctx.displayExtent();
        return true;
    }

private:
    void record(FgContext& ctx, VkCommandBuffer cmd, uint32_t cur) {
        if (written_ == frame_) return;  // once per frame (synthesis or real output first)
        auto& vt = ctx.device().vt;
        const Config cfg = ctx.effectiveConfig();
        prevValid_ = written_ + 1 == frame_;
        const bool history = prevValid_ && flowFrame_ == frame_;
        h_ = written_ == ~0ull ? 0 : 1 - h_;
        written_ = frame_;
        const FgFineLevel l0 = ctx.fineLevel();
        const VkExtent2D disp = ctx.displayExtent(), rend = ctx.renderExtent();
        const float sx = float(disp.width) / float(rend.width);
        const float sy = float(disp.height) / float(rend.height);
        const float fs = ctx.flowToRender();
        TaaPC pc{{(int32_t)disp.width, (int32_t)disp.height},
                 {(int32_t)rend.width, (int32_t)rend.height},
                 {1.f / (l0.fine.width * float(l0.block) * fs * sx), 1.f / (l0.fine.height * float(l0.block) * fs * sy)},
                 fs * sx, cfg.upscaleTemporal, cfg.sceneCutLow, cfg.sceneCutHigh,
                 std::clamp(cfg.upscaleFilter, 0, 2), history ? 1 : 0, ctx.repeatFlagValid() ? 1 : 0};
        vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.pipeline);
        vt.CmdPushConstants(cmd, pipe_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.layout, 0, 1, &accum_[cur * 2 + h_], 0, nullptr);
        vt.CmdDispatch(cmd, fg::divUp(disp.width, 16), fg::divUp(disp.height, 16), 1);
        ctx.barrier(cmd);
    }

    FgPipeline pipe_;
    AllocatedImage hist_[2];
    std::vector<VkDescriptorSet> accum_;  // [source * 2 + h]: writes hist_[h]
    VkDescriptorSet output_[2] = {};      // the real frame's output from hist_[h]
    std::vector<VkDescriptorSet> synth_;  // [h * outputs + output]: prev hist_[1-h], cur hist_[h]
    uint64_t frame_ = 0;                  // counted by frameBegin
    uint64_t flowFrame_ = ~0ull;          // frame whose flow was computed
    uint64_t written_ = ~0ull;            // frame whose history was written
    uint32_t h_ = 0;                      // history written last
    bool prevValid_ = false;              // hist_[1 - h_] holds the previous frame
};

} // namespace

std::unique_ptr<FgPass> makeTemporalUpscalePass() { return std::make_unique<TemporalUpscalePass>(); }

} // namespace bdex
