// false_detail: over each finished generated frame, before the fps HUD,
// the fine detail that neither input has anywhere near leans to its local
// average (shaders/false_detail.comp: the edges between neighbouring blocks
// that took different vectors). Packed 8- and 10-bit SDR formats, no
// upscaling. The synthesis's inputs and t are noted before each
// synthesis (synthesisInputs, beforeSynthesis), the filter runs when the
// output is done (afterOutput).
#include "fg_pass.h"
#include "framegen_internal.h"
#include "shaders.h"

#include <algorithm>
#include <cmath>

namespace bdex {
namespace {

struct FdPC { int32_t size[2]; uint32_t encoding; int32_t mode; float strength; float ratio; float guard; int32_t blur; };

class FalseDetailPass : public FgPass {
public:
    const char* name() const override { return "false_detail"; }
    bool active(const Config& cfg) const override { return usable_ && cfg.falseDetail > 0.f; }

    void create(FgContext& ctx) override {
        const OutputEncoding enc = ctx.outputEncoding();
        const uint32_t e = enc.encoding & 0xffu;
        // (an sRGB format's stored bytes are perceptual too: the shader reads them as such)
        usable_ = !enc.is64 && e <= 3 && (enc.transfer == TRANSFER_PERCEPTUAL || enc.srgb) && !ctx.upscaling();
        if (!usable_) return;
        const VkDescriptorType SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pipe_ = ctx.makePipeline(false_detail_comp, false_detail_comp_size, {SI, SI, SI, SI, CIS, CIS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER});
        const VkExtent2D d = ctx.displayExtent();
        const VkExtent2D c{fg::divUp(d.width, kCell), fg::divUp(d.height, kCell)};
        scratch_ = ctx.createImage(VK_FORMAT_R32_UINT, d, VK_IMAGE_USAGE_STORAGE_BIT);
        cells_ = ctx.createImage(VK_FORMAT_R32_SFLOAT, c, VK_IMAGE_USAGE_STORAGE_BIT);
        cellsMax_ = ctx.createImage(VK_FORMAT_R32_SFLOAT, c, VK_IMAGE_USAGE_STORAGE_BIT);
        tiles_ = ctx.device().createBuffer(4 * VkDeviceSize(fg::divUp(d.width, 16)) * fg::divUp(d.height, 16),
                                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        // The average's radius follows the flow's block size on screen: 2 at
        // the display's resolution, 4 at half (1440p quality: 1440p
        // recordings +0.20 / +0.13 dB instead of +0.16 / +0.10).
        blur_ = std::clamp(int(std::lround(2.f * ctx.flowToRender() * float(d.width) / float(ctx.renderExtent().width))), 2, 4);
        n_ = ctx.sourceCount();
        for (uint32_t o = 0; o < ctx.outputs(); ++o)
            for (uint32_t prev = 0; prev < n_; ++prev)
                for (uint32_t cur = 0; cur < n_; ++cur) {
                    VkDescriptorSet s = ctx.allocSet(pipe_.setLayout);
                    ctx.writeImage(s, 0, SI, ctx.outputView(int(o)));
                    ctx.writeImage(s, 1, SI, scratch_.view);
                    ctx.writeImage(s, 2, SI, cells_.view);
                    ctx.writeImage(s, 3, SI, cellsMax_.view);
                    ctx.writeImage(s, 4, CIS, ctx.sourceView(prev));
                    ctx.writeImage(s, 5, CIS, ctx.sourceView(cur));
                    ctx.writeBuffer(s, 6, tiles_.buffer);
                    sets_.push_back(s);
                }
        pending_.assign(ctx.outputs(), Pending{});
    }
    void destroy(FgContext& ctx) override {
        if (!usable_) return;
        for (AllocatedImage* img : {&scratch_, &cells_, &cellsMax_})
            if (img->image) ctx.device().destroyImage(*img);
        if (tiles_.buffer) ctx.device().destroyBuffer(tiles_);
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
    void beforeSynthesis(FgContext&, VkCommandBuffer, float t, int32_t, bool, const Config& cfg, SynthesisInputs&) override {
        if (usable_ && next_ < pending_.size()) {
            // Interpolated and predicted frames alike (extrapolation, 15/10:
            // every sequence up, +0.15 dB on average, flicker down to -1.1).
            pending_[next_].on = cfg.falseDetail > 0.f;
            pending_[next_].predicted = t > 1.f;
            pending_[next_].strength = cfg.falseDetail;
        }
    }

    void afterOutput(FgContext& ctx, VkCommandBuffer cmd, int output) override {
        if (!usable_ || output < 0 || size_t(output) >= pending_.size() || !pending_[size_t(output)].on) return;
        const Pending& p = pending_[size_t(output)];
        auto& vt = ctx.device().vt;
        const VkExtent2D d = ctx.displayExtent();
        const VkExtent2D c{fg::divUp(d.width, kCell), fg::divUp(d.height, kCell)};
        VkDescriptorSet set = sets_[(size_t(output) * n_ + p.prev) * n_ + p.cur];
        FdPC pc{{(int32_t)d.width, (int32_t)d.height}, ctx.outputEncoding().encoding, 0, p.strength, p.predicted ? kRatioPredicted : kRatio, kGuard, blur_};
        vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.pipeline);
        vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.layout, 0, 1, &set, 0, nullptr);
        // The inputs' detail map: once a frame (every output of a frame has
        // the same two inputs).
        const bool fresh = cellsFrame_ != frame_ || cellsPrev_ != p.prev || cellsCur_ != p.cur;
        for (int mode : {1, 2, 0, 3}) {
            if ((mode == 1 || mode == 2) && !fresh) continue;
            pc.mode = mode;
            vt.CmdPushConstants(cmd, pipe_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            const VkExtent2D g = mode == 2 ? c : d;
            vt.CmdDispatch(cmd, fg::divUp(g.width, 16), fg::divUp(g.height, 16), 1);
            ctx.barrier(cmd);
        }
        cellsFrame_ = frame_;
        cellsPrev_ = p.prev;
        cellsCur_ = p.cur;
        pending_[size_t(output)].on = false;
        next_ = ~0u;  // (online_tune's probe synthesises too: only the next recordInterpolate counts)
    }

private:
    static constexpr uint32_t kCell = 4;  // as the shader's
    // The generated detail must exceed this many times the inputs' (a
    // predicted frame's mosaic is coarser: 1.25 there, extrapolation +0.023 dB
    // over 1.5, every sequence up).
    static constexpr float kRatio = 1.5f, kRatioPredicted = 1.25f;
    static constexpr float kGuard = 8.f;  // (mean |Laplacian| over 3x3 in both inputs, 8-bit units)
    struct Pending { uint32_t prev = 0, cur = 0; float strength = 0.f; bool on = false; bool predicted = false; };
    bool usable_ = false;
    FgPipeline pipe_;
    AllocatedImage scratch_, cells_, cellsMax_;
    AllocatedBuffer tiles_;  // per 16x16 tile of the output: anything filtered
    uint64_t frame_ = 0, cellsFrame_ = ~0ull;
    uint32_t cellsPrev_ = ~0u, cellsCur_ = ~0u;
    uint32_t n_ = 0;
    int32_t blur_ = 2;
    std::vector<VkDescriptorSet> sets_;  // [(output * sources + prev) * sources + cur]
    std::vector<Pending> pending_;
    uint32_t next_ = ~0u;
};

} // namespace

std::unique_ptr<FgPass> makeFalseDetailPass() { return std::make_unique<FalseDetailPass>(); }

} // namespace bdex
