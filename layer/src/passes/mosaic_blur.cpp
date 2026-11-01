// mosaic_blur: over each finished interpolated frame, after false_detail,
// the pixels where the finest flow is chaos (most vectors around them far
// from their neighbours': no motion explains the content, the synthesis
// assembles a mosaic) lean to the local average of their mix with the inputs
// warped along the smoothed flow (shaders/mosaic_blur.comp). The chaos
// density and the smoothed flow are computed once a frame; packed 8- and
// 10-bit formats, perceptual encodings, no upscaling (as false_detail).
#include "fg_pass.h"
#include "framegen_internal.h"
#include "shaders.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace bdex {
namespace {

struct MbPC { int32_t size[2]; uint32_t encoding; int32_t mode; float strength; float threshold; float deviation; int32_t radius; float toFlow[2]; int32_t agree;
              float flowScale; float flowInvSize[2]; float t; int32_t timing; };
static_assert(offsetof(MbPC, flowInvSize) % 8 == 0, "a vec2 of the shader's push constants: 8-byte aligned");

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
        pipe_ = ctx.makePipeline(mosaic_blur_comp, mosaic_blur_comp_size, {SI, SI, SI, CIS, CIS, SI, CIS, CIS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER});
        const VkExtent2D d = ctx.displayExtent();
        const FgFineLevel fine = ctx.fineLevel();
        fineExtent_ = fine.fine;
        scratch_ = ctx.createImage(VK_FORMAT_R32_UINT, d, VK_IMAGE_USAGE_STORAGE_BIT);
        density_ = ctx.createImage(VK_FORMAT_R32_SFLOAT, fineExtent_, VK_IMAGE_USAGE_STORAGE_BIT);
        smooth_ = ctx.createImage(VK_FORMAT_R32G32B32A32_SFLOAT, fineExtent_, VK_IMAGE_USAGE_STORAGE_BIT);
        // Chaos: a vector about 1 % of the picture's width from a neighbour's
        // (8 display pixels at 853, measured on recordings at that size), in
        // flow units; the average over half a flow block each side (5x5 at
        // the display's resolution, 9x9 where the flow is at half: a texel is
        // at least `radius` pixels wide, as mode 1's probes need).
        const VkExtent2D r = ctx.renderExtent();
        const float f2r = ctx.flowToRender(), sx = float(d.width) / float(r.width), sy = float(d.height) / float(r.height);
        deviation_ = 0.0094f * float(r.width) / std::max(f2r, 1e-3f);
        radius_ = std::clamp(int(std::lround(2.f * f2r * sx)), 1, 4);
        flowScale_ = f2r * sx;
        flowInvSize_[0] = 1.f / (float(fine.fine.width) * float(fine.block) * f2r * sx);
        flowInvSize_[1] = 1.f / (float(fine.fine.height) * float(fine.block) * f2r * sy);
        n_ = ctx.sourceCount();
        for (uint32_t o = 0; o < ctx.outputs(); ++o)
            for (uint32_t prev = 0; prev < n_; ++prev)
                for (uint32_t cur = 0; cur < n_; ++cur) {
                    VkDescriptorSet s = ctx.allocSet(pipe_.setLayout);
                    ctx.writeImage(s, 0, SI, ctx.outputView(int(o)));
                    ctx.writeImage(s, 1, SI, scratch_.view);
                    ctx.writeImage(s, 2, SI, density_.view);
                    ctx.writeImage(s, 3, CIS, fine.flow[0]);
                    ctx.writeImage(s, 4, CIS, fine.flow[1]);
                    ctx.writeImage(s, 5, SI, smooth_.view);
                    ctx.writeImage(s, 6, CIS, ctx.sourceView(prev));
                    ctx.writeImage(s, 7, CIS, ctx.sourceView(cur));
                    ctx.writeBuffer(s, 8, ctx.stateBuffer());
                    sets_.push_back(s);
                }
        pending_.assign(ctx.outputs(), Pending{});
    }
    void destroy(FgContext& ctx) override {
        if (!usable_) return;
        if (scratch_.image) ctx.device().destroyImage(scratch_);
        if (density_.image) ctx.device().destroyImage(density_);
        if (smooth_.image) ctx.device().destroyImage(smooth_);
        ctx.destroyPipeline(pipe_);
    }

    void frameBegin(FgContext&) override { ++frame_; }

    VkDescriptorSet synthesisInputs(FgContext&, VkCommandBuffer, uint32_t prev, uint32_t cur, uint32_t output) override {
        if (usable_ && output < pending_.size()) {
            pending_[output].prev = prev;
            pending_[output].cur = cur;
            next_ = output;
        }
        return VK_NULL_HANDLE;
    }
    void beforeSynthesis(FgContext&, VkCommandBuffer, float t, int32_t timing, bool, const Config& cfg, SynthesisInputs&) override {
        if (usable_ && next_ < pending_.size()) {
            pending_[next_].on = cfg.mosaicBlur > 0.f && t <= 1.f;  // (interpolated frames: measured there only)
            pending_[next_].strength = cfg.mosaicBlur;
            pending_[next_].t = t;
            pending_[next_].timing = timing;
        }
    }

    void afterOutput(FgContext& ctx, VkCommandBuffer cmd, int output) override {
        if (!usable_ || output < 0 || size_t(output) >= pending_.size() || !pending_[size_t(output)].on) return;
        const Pending& pd = pending_[size_t(output)];
        auto& vt = ctx.device().vt;
        const VkExtent2D d = ctx.displayExtent();
        MbPC pc{{(int32_t)d.width, (int32_t)d.height}, ctx.outputEncoding().encoding, 0, pd.strength,
                kThreshold, deviation_, radius_,
                {float(fineExtent_.width) / float(d.width), float(fineExtent_.height) / float(d.height)},
                kAgree, flowScale_, {flowInvSize_[0], flowInvSize_[1]}, pd.t, pd.timing};
        VkDescriptorSet set = sets_[(size_t(output) * n_ + pd.prev) * n_ + pd.cur];
        vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.pipeline);
        vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.layout, 0, 1, &set, 0, nullptr);
        if (densityFrame_ != frame_) {  // once a frame: every output has the same flow (density, smoothed flow)
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
    struct Pending { uint32_t prev = 0, cur = 0; float strength = 0.f, t = 0.5f; int32_t timing = 0; bool on = false; };
    bool usable_ = false;
    FgPipeline pipe_;
    AllocatedImage scratch_, density_, smooth_;
    VkExtent2D fineExtent_{};
    float deviation_ = 8.f;
    int32_t radius_ = 2;
    float flowScale_ = 1.f, flowInvSize_[2] = {1.f, 1.f};
    uint32_t n_ = 0;
    uint64_t frame_ = 0, densityFrame_ = ~0ull;
    std::vector<VkDescriptorSet> sets_;  // [(output * sources + prev) * sources + cur]
    std::vector<Pending> pending_;
    uint32_t next_ = ~0u;
};

} // namespace

std::unique_ptr<FgPass> makeMosaicBlurPass() { return std::make_unique<MosaicBlurPass>(); }

} // namespace bdex
