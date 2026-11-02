// hud_layer: over each finished interpolated frame, after mosaic_blur, where
// hud_mask's static_soft evidence (a HUD's anti-aliased text holding still
// over the moving scene) is dense, the in-place cross-fade of the inputs plus
// the low frequencies of the synthesis's difference to it
// (shaders/hud_layer.comp). The weight is computed once a frame from the
// evidence; packed 8- and 10-bit formats, perceptual encodings, no upscaling
// (as mosaic_blur), with hud_mask on.
#include "fg_pass.h"
#include "framegen_internal.h"
#include "shaders.h"

#include <algorithm>
#include <cmath>

namespace bdex {
namespace {

struct HlPC { int32_t size[2]; uint32_t encoding; int32_t mode; float strength; float t; int32_t timing;
              int32_t dilate; int32_t dilateStep; int32_t cell; int32_t radius; int32_t stride; int32_t margin; float rate; };

class HudLayerPass : public FgPass {
public:
    const char* name() const override { return "hud_layer"; }
    bool active(const Config& cfg) const override { return usable_ && cfg.hudLayer > 0.f && cfg.hudMask > 0; }

    void create(FgContext& ctx) override {
        const OutputEncoding enc = ctx.outputEncoding();
        const uint32_t e = enc.encoding & 0xffu;
        usable_ = !enc.is64 && e <= 3 && (enc.transfer == TRANSFER_PERCEPTUAL || enc.transfer == TRANSFER_PQ || enc.srgb) && !ctx.upscaling();
        if (!usable_) return;
        const VkDescriptorType SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pipe_ = ctx.makePipeline(hud_layer_comp, hud_layer_comp_size, {SI, SI, SI, SI, CIS, CIS, CIS, SB, SB});
        const VkExtent2D d = ctx.displayExtent();
        scratch_ = ctx.createImage(VK_FORMAT_R32_UINT, d, VK_IMAGE_USAGE_STORAGE_BIT);
        // Measured at 1280 pixels across (and kept below: 853-wide recordings
        // as well): on cells of 4 pixels (the evidence every 2nd), the
        // maximum over 2 cells around, the average over 3x3 cells; the
        // difference's average over 9x9 (every 2nd); twice those reaches in
        // pixels at 1440p.
        const float s = std::max(1.f, float(d.width) / 1280.f);
        dilate_ = 2;
        dilateStep_ = std::max<int32_t>(1, int32_t(std::lround(2.f * s)));
        cell_ = std::max<int32_t>(2, int32_t(std::lround(4.f * s)));
        radius_ = int32_t(std::lround(4.f * s));
        stride_ = std::max<int32_t>(1, radius_ / 2);
        margin_ = int32_t(std::lround(4.f * s));
        const VkExtent2D cells{fg::divUp(d.width, uint32_t(cell_)), fg::divUp(d.height, uint32_t(cell_))};
        dilated_ = ctx.createImage(VK_FORMAT_R32_SFLOAT, cells, VK_IMAGE_USAGE_STORAGE_BIT);
        weight_ = ctx.createImage(VK_FORMAT_R32_SFLOAT, cells, VK_IMAGE_USAGE_STORAGE_BIT);
        cells_ = cells;
        cellMax_ = ctx.device().createBuffer(4 * VkDeviceSize(cells.width) * cells.height, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        n_ = ctx.sourceCount();
        for (uint32_t o = 0; o < ctx.outputs(); ++o)
            for (uint32_t prev = 0; prev < n_; ++prev)
                for (uint32_t cur = 0; cur < n_; ++cur) {
                    VkDescriptorSet set = ctx.allocSet(pipe_.setLayout);
                    ctx.writeImage(set, 0, SI, ctx.outputView(int(o)));
                    ctx.writeImage(set, 1, SI, scratch_.view);
                    ctx.writeImage(set, 2, SI, dilated_.view);
                    ctx.writeImage(set, 3, SI, weight_.view);
                    ctx.writeImage(set, 4, CIS, ctx.staticMask());
                    ctx.writeImage(set, 5, CIS, ctx.sourceView(prev));
                    ctx.writeImage(set, 6, CIS, ctx.sourceView(cur));
                    ctx.writeBuffer(set, 7, ctx.stateBuffer());
                    ctx.writeBuffer(set, 8, cellMax_.buffer);
                    sets_.push_back(set);
                }
        pending_.assign(ctx.outputs(), Pending{});
    }
    void destroy(FgContext& ctx) override {
        if (!usable_) return;
        for (AllocatedImage* i : {&scratch_, &dilated_, &weight_})
            if (i->image) ctx.device().destroyImage(*i);
        if (cellMax_.buffer) ctx.device().destroyBuffer(cellMax_);
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
            // (interpolated frames: measured there only)
            pending_[next_].on = cfg.hudLayer > 0.f && cfg.hudMask > 0 && t <= 1.f;
            pending_[next_].strength = cfg.hudLayer;
            pending_[next_].t = t;
            pending_[next_].timing = timing;
        }
    }

    void afterOutput(FgContext& ctx, VkCommandBuffer cmd, int output) override {
        if (!usable_ || output < 0 || size_t(output) >= pending_.size() || !pending_[size_t(output)].on) return;
        const Pending& pd = pending_[size_t(output)];
        auto& vt = ctx.device().vt;
        const VkExtent2D d = ctx.displayExtent();
        HlPC pc{{(int32_t)d.width, (int32_t)d.height}, ctx.outputEncoding().encoding, 0, pd.strength, pd.t, pd.timing,
                dilate_, dilateStep_, cell_, radius_, stride_, margin_,
                // (from zero on the first frame, and after frames without this
                // pass: what the average held is stale)
                weightFrame_ == ~0ull || frame_ > weightFrame_ + 2 ? -kRate : kRate};
        VkDescriptorSet set = sets_[(size_t(output) * n_ + pd.prev) * n_ + pd.cur];
        vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.pipeline);
        vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.layout, 0, 1, &set, 0, nullptr);
        for (int mode : {0, 1, 4, 2, 3}) {
            if ((mode < 2 || mode == 4) && weightFrame_ == frame_) continue;  // once a frame: the evidence is the frame's
            pc.mode = mode;
            vt.CmdPushConstants(cmd, pipe_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            const VkExtent2D g = mode < 2 || mode == 4 ? cells_ : d;
            vt.CmdDispatch(cmd, fg::divUp(g.width, 16), fg::divUp(g.height, 16), 1);
            ctx.barrier(cmd);
        }
        weightFrame_ = frame_;
        pending_[size_t(output)].on = false;
        next_ = ~0u;  // (online_tune's probe synthesises too: only the next recordInterpolate counts)
    }

private:
    struct Pending { uint32_t prev = 0, cur = 0; float strength = 0.f, t = 0.5f; int32_t timing = 0; bool on = false; };
    bool usable_ = false;
    FgPipeline pipe_;
    AllocatedImage scratch_, dilated_, weight_;
    AllocatedBuffer cellMax_;  // per cell (a buffer: a stage may count on 4 storage images only)
    // The persistent evidence's rate a frame (0.1: two thirds of the way in
    // ten frames, a third of a second at 30 fps).
    static constexpr float kRate = 0.1f;
    int32_t dilate_ = 6, dilateStep_ = 2, cell_ = 4, radius_ = 4, stride_ = 2, margin_ = 4;
    VkExtent2D cells_{};
    uint64_t frame_ = 0, weightFrame_ = ~0ull;
    uint32_t n_ = 0;
    std::vector<VkDescriptorSet> sets_;  // [(output * sources + prev) * sources + cur]
    std::vector<Pending> pending_;
    uint32_t next_ = ~0u;
};

} // namespace

std::unique_ptr<FgPass> makeHudLayerPass() { return std::make_unique<HudLayerPass>(); }

} // namespace bdex
