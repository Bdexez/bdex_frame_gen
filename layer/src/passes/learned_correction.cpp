// learned_correction: the network of tools/train_correction.py over each
// finished generated frame (shaders/learned_correction.comp), before the fps
// HUD — 8-bit outputs at render size only (what it was trained on). The
// synthesis's inputs and t are noted before each synthesis
// (synthesisInputs, beforeSynthesis), the correction runs when the output is
// done (afterOutput).
#include "fg_pass.h"
#include "framegen_internal.h"
#include "shaders.h"

namespace bdex {
namespace {

struct LcPC { int32_t size[2]; uint32_t encoding; int32_t mode; float t; float strength; };

class LearnedCorrectionPass : public FgPass {
public:
    const char* name() const override { return "learned_correction"; }
    bool active(const Config& cfg) const override { return usable_ && cfg.learnedCorrection > 0.f; }

    void create(FgContext& ctx) override {
        const OutputEncoding enc = ctx.outputEncoding();
        const uint32_t e = enc.encoding & 0xffu;
        usable_ = !enc.is64 && (e == 0 || e == 1) && !ctx.upscaling();
        if (!usable_) return;
        const VkDescriptorType SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pipe_ = ctx.makePipeline(learned_correction_comp, learned_correction_comp_size, {SI, SI, CIS, CIS});
        scratch_ = ctx.createImage(VK_FORMAT_R32_UINT, ctx.displayExtent(), VK_IMAGE_USAGE_STORAGE_BIT);
        n_ = ctx.sourceCount();
        for (uint32_t o = 0; o < ctx.outputs(); ++o)
            for (uint32_t prev = 0; prev < n_; ++prev)
                for (uint32_t cur = 0; cur < n_; ++cur) {
                    VkDescriptorSet s = ctx.allocSet(pipe_.setLayout);
                    ctx.writeImage(s, 0, SI, ctx.outputView(int(o)));
                    ctx.writeImage(s, 1, SI, scratch_.view);
                    ctx.writeImage(s, 2, CIS, ctx.sourceView(prev));
                    ctx.writeImage(s, 3, CIS, ctx.sourceView(cur));
                    sets_.push_back(s);
                }
        pending_.assign(ctx.outputs(), Pending{});
    }
    void destroy(FgContext& ctx) override {
        if (!usable_) return;
        if (scratch_.image) ctx.device().destroyImage(scratch_);
        ctx.destroyPipeline(pipe_);
    }

    VkDescriptorSet synthesisInputs(FgContext&, VkCommandBuffer, uint32_t prev, uint32_t cur, uint32_t output) override {
        if (usable_ && output < pending_.size()) {
            pending_[output].prev = prev;
            pending_[output].cur = cur;
            next_ = output;
        }
        return VK_NULL_HANDLE;
    }
    void beforeSynthesis(FgContext&, VkCommandBuffer, float t, int32_t, bool, const Config& cfg, SynthesisInputs&) override {
        if (usable_ && next_ < pending_.size()) {
            pending_[next_].t = t;
            pending_[next_].on = cfg.learnedCorrection > 0.f;  // (both modes: one set of weights each)
            pending_[next_].strength = cfg.learnedCorrection;
        }
    }

    void afterOutput(FgContext& ctx, VkCommandBuffer cmd, int output) override {
        if (!usable_ || output < 0 || size_t(output) >= pending_.size() || !pending_[size_t(output)].on) return;
        const Pending& p = pending_[size_t(output)];
        auto& vt = ctx.device().vt;
        const VkExtent2D d = ctx.displayExtent();
        VkDescriptorSet set = sets_[(size_t(output) * n_ + p.prev) * n_ + p.cur];
        LcPC pc{{(int32_t)d.width, (int32_t)d.height}, ctx.outputEncoding().encoding, 0, p.t, p.strength};
        vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.pipeline);
        vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.layout, 0, 1, &set, 0, nullptr);
        for (int mode = 0; mode < 2; ++mode) {
            pc.mode = mode;
            vt.CmdPushConstants(cmd, pipe_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            vt.CmdDispatch(cmd, fg::divUp(d.width, 16), fg::divUp(d.height, 16), 1);
            ctx.barrier(cmd);
        }
        pending_[size_t(output)].on = false;
        next_ = ~0u;  // (online_tune's probe synthesises too: only the next recordInterpolate counts)
    }

private:
    struct Pending { uint32_t prev = 0, cur = 0; float t = 0.5f, strength = 0.f; bool on = false; };
    bool usable_ = false;
    FgPipeline pipe_;
    AllocatedImage scratch_;
    uint32_t n_ = 0;
    std::vector<VkDescriptorSet> sets_;  // [(output * sources + prev) * sources + cur]
    std::vector<Pending> pending_;
    uint32_t next_ = ~0u;
};

} // namespace

std::unique_ptr<FgPass> makeLearnedCorrectionPass() { return std::make_unique<LearnedCorrectionPass>(); }

} // namespace bdex
