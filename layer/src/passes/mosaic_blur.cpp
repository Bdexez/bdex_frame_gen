// mosaic_blur: over each finished interpolated frame, after false_detail,
// the pixels where the finest flow is chaos (most vectors around them far
// from their neighbours': no motion explains the content, the synthesis
// assembles a mosaic) lean to their local average (shaders/mosaic_blur.comp).
// The chaos density is computed once a frame from the forward flow; packed
// 8- and 10-bit formats, perceptual encodings, no upscaling (as false_detail).
#include "fg_pass.h"
#include "framegen_internal.h"
#include "shaders.h"

#include <algorithm>
#include <cmath>

namespace bdex {
namespace {

struct MbPC { int32_t size[2]; uint32_t encoding; int32_t mode; float strength; float threshold; float deviation; int32_t radius; float toFlow[2]; int32_t agree; };

class MosaicBlurPass : public FgPass {
public:
    const char* name() const override { return "mosaic_blur"; }
    bool active(const Config& cfg) const override { return usable_ && cfg.mosaicBlur > 0.f; }

    void create(FgContext& ctx) override {
        const OutputEncoding enc = ctx.outputEncoding();
        const uint32_t e = enc.encoding & 0xffu;
        usable_ = !enc.is64 && e <= 3 && (enc.transfer == TRANSFER_PERCEPTUAL || enc.transfer == TRANSFER_PQ || enc.srgb) && !ctx.upscaling();
        if (!usable_) return;
        const VkDescriptorType SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pipe_ = ctx.makePipeline(mosaic_blur_comp, mosaic_blur_comp_size, {SI, SI, SI, CIS});
        const VkExtent2D d = ctx.displayExtent();
        const FgFineLevel fine = ctx.fineLevel();
        fineExtent_ = fine.fine;
        scratch_ = ctx.createImage(VK_FORMAT_R32_UINT, d, VK_IMAGE_USAGE_STORAGE_BIT);
        density_ = ctx.createImage(VK_FORMAT_R32_SFLOAT, fineExtent_, VK_IMAGE_USAGE_STORAGE_BIT);
        // Chaos: a vector about 1 % of the picture's width from a neighbour's
        // (8 display pixels at 853, measured on recordings at that size), in
        // flow units; the average over one flow block (3x3 at the display's
        // resolution, 5x5 where the flow is at half).
        const VkExtent2D r = ctx.renderExtent();
        deviation_ = 0.0094f * float(r.width) / std::max(ctx.flowToRender(), 1e-3f);
        radius_ = std::clamp(int(std::lround(ctx.flowToRender() * float(d.width) / float(r.width))), 1, 3);
        for (uint32_t o = 0; o < ctx.outputs(); ++o) {
            VkDescriptorSet s = ctx.allocSet(pipe_.setLayout);
            ctx.writeImage(s, 0, SI, ctx.outputView(int(o)));
            ctx.writeImage(s, 1, SI, scratch_.view);
            ctx.writeImage(s, 2, SI, density_.view);
            ctx.writeImage(s, 3, CIS, fine.flow[0]);
            sets_.push_back(s);
        }
        pending_.assign(ctx.outputs(), Pending{});
    }
    void destroy(FgContext& ctx) override {
        if (!usable_) return;
        if (scratch_.image) ctx.device().destroyImage(scratch_);
        if (density_.image) ctx.device().destroyImage(density_);
        ctx.destroyPipeline(pipe_);
    }

    void frameBegin(FgContext&) override { ++frame_; }

    VkDescriptorSet synthesisInputs(FgContext&, VkCommandBuffer, uint32_t, uint32_t, uint32_t output) override {
        if (usable_ && output < pending_.size()) next_ = output;
        return VK_NULL_HANDLE;
    }
    void beforeSynthesis(FgContext&, VkCommandBuffer, float t, int32_t, bool, const Config& cfg, SynthesisInputs&) override {
        if (usable_ && next_ < pending_.size()) {
            pending_[next_].on = cfg.mosaicBlur > 0.f && t <= 1.f;  // (interpolated frames: measured there only)
            pending_[next_].strength = cfg.mosaicBlur;
        }
    }

    void afterOutput(FgContext& ctx, VkCommandBuffer cmd, int output) override {
        if (!usable_ || output < 0 || size_t(output) >= pending_.size() || !pending_[size_t(output)].on) return;
        auto& vt = ctx.device().vt;
        const VkExtent2D d = ctx.displayExtent();
        MbPC pc{{(int32_t)d.width, (int32_t)d.height}, ctx.outputEncoding().encoding, 0, pending_[size_t(output)].strength,
                kThreshold, deviation_, radius_,
                {float(fineExtent_.width) / float(d.width), float(fineExtent_.height) / float(d.height)},
                kAgree};
        vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.pipeline);
        vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.layout, 0, 1, &sets_[size_t(output)], 0, nullptr);
        if (densityFrame_ != frame_) {  // once a frame: every output has the same flow
            MbPC p0 = pc;
            p0.size[0] = int32_t(fineExtent_.width);
            p0.size[1] = int32_t(fineExtent_.height);
            vt.CmdPushConstants(cmd, pipe_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p0), &p0);
            vt.CmdDispatch(cmd, fg::divUp(fineExtent_.width, 16), fg::divUp(fineExtent_.height, 16), 1);
            ctx.barrier(cmd);
            densityFrame_ = frame_;
        }
        for (int mode : {1, 2}) {
            pc.mode = mode;
            vt.CmdPushConstants(cmd, pipe_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            vt.CmdDispatch(cmd, fg::divUp(d.width, 16), fg::divUp(d.height, 16), 1);
            ctx.barrier(cmd);
        }
        pending_[size_t(output)].on = false;
        next_ = ~0u;  // (online_tune's probe synthesises too: only the next recordInterpolate counts)
    }

private:
    // Chaos: a vector with fewer than kAgree of its 8 neighbours within the
    // deviation, over half the texels around (17/10, recordings at x2 /
    // game-like scenes at x4: 7 and 0.5 — PRAGMATA +0.14 dB, flicker -0.17,
    // Oblivion +0.07, Cyberpunk +0.03, the scenes' whole frames +0.01 to
    // +0.02 but their moving edges -0.5 to +0.01; 6: half the gains, edges
    // to -0.2; 5 or less: hardly ever; any neighbour far (8) with 0.3:
    // walk -0.9 dB, a zoom's flow is no mosaic). Off by default for those edges.
    static constexpr float kThreshold = 0.5f;
    static constexpr int32_t kAgree = 7;
    struct Pending { float strength = 0.f; bool on = false; };
    bool usable_ = false;
    FgPipeline pipe_;
    AllocatedImage scratch_, density_;
    VkExtent2D fineExtent_{};
    float deviation_ = 8.f;
    int32_t radius_ = 1;
    uint64_t frame_ = 0, densityFrame_ = ~0ull;
    std::vector<VkDescriptorSet> sets_;  // per output
    std::vector<Pending> pending_;
    uint32_t next_ = ~0u;
};

} // namespace

std::unique_ptr<FgPass> makeMosaicBlurPass() { return std::make_unique<MosaicBlurPass>(); }

} // namespace bdex
