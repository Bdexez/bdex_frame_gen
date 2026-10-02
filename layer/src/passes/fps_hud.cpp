// The fps HUD (hud / overlay option): drawn over each finished output
// image by shaders/hud_overlay.comp, after upscaling and sharpening.
#include "fg_pass.h"
#include "framegen_internal.h"
#include "shaders.h"

#include <algorithm>

namespace bdex {
namespace {

struct HudPC {
    int32_t size[2];
    uint32_t encoding;
    int32_t mode, game, out, low1, low01, latency;
    uint32_t graph[FgHud::kHudGraph / 4];  // level 6
    int32_t graphRef;
};
static_assert(sizeof(HudPC) <= fg::kPushSize);

// The HUD box: 3x5 glyphs at scale 4 from (14, 14) (common.glsl, hudPixel),
// at most 10 cells and 4 lines: the box ends before (174, 110).
constexpr uint32_t kHudW = 176 + 16, kHudH = 96 + 16;
// Level 6 adds the pacing graph under it: 192 x 70 plus its padding.
constexpr uint32_t kGraphW = 14 + 192 + 8 + 8, kGraphH = kHudH + 8 + 70 + 8 + 8;

class FpsHudPass : public FgPass {
public:
    const char* name() const override { return "hud"; }

    void create(FgContext& ctx) override {
        const bool is64 = ctx.outputEncoding().is64;
        pipe_ = is64 ? ctx.makePipeline(hud_overlay_comp_u64, hud_overlay_comp_u64_size, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE})
                     : ctx.makePipeline(hud_overlay_comp_u32, hud_overlay_comp_u32_size, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE});
        for (int o = -1; o < int(ctx.outputs()); ++o) {
            VkDescriptorSet s = ctx.allocSet(pipe_.setLayout);
            ctx.writeImage(s, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, ctx.outputView(o));
            sets_.push_back(s);
        }
    }
    void destroy(FgContext& ctx) override { ctx.destroyPipeline(pipe_); }

    void afterOutput(FgContext& ctx, VkCommandBuffer cmd, int output) override {
        auto& vt = ctx.device().vt;
        const VkExtent2D d = ctx.displayExtent();
        const FgHud h = ctx.hud();
        HudPC pc{{(int32_t)d.width, (int32_t)d.height}, ctx.outputEncoding().encoding, h.mode, h.game, h.out, h.low1,
                 h.low01, h.latency, {}, h.graphRef};
        for (int i = 0; i < FgHud::kHudGraph / 4; ++i) pc.graph[i] = h.graph[i];
        const uint32_t w = h.mode >= 6 ? std::max(kHudW, kGraphW) : kHudW, hh = h.mode >= 6 ? kGraphH : kHudH;
        vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.pipeline);
        vt.CmdPushConstants(cmd, pipe_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.layout, 0, 1, &sets_[output + 1], 0, nullptr);
        vt.CmdDispatch(cmd, fg::divUp(std::min(w, d.width), 16), fg::divUp(std::min(hh, d.height), 16), 1);
        ctx.barrier(cmd);
    }

private:
    FgPipeline pipe_;
    std::vector<VkDescriptorSet> sets_;  // [output + 1]: the real frame's image first
};

} // namespace

std::unique_ptr<FgPass> makeFpsHudPass() { return std::make_unique<FpsHudPass>(); }

} // namespace bdex
