// hud_mask: the learned static-overlay mask (see shaders/hud_mask.comp),
// updated once per generated real frame and bound to every synthesis
// (FgContext::setStaticMask), which keeps masked pixels as they are.
#include "fg_pass.h"
#include "framegen_internal.h"
#include "shaders.h"

namespace bdex {
namespace {

struct HudMaskPC { int32_t size[2]; int32_t slot; uint32_t moving; };

class HudMaskPass : public FgPass {
public:
    const char* name() const override { return "hud_mask"; }

    void create(FgContext& ctx) override {
        const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                               SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        DeviceData& dev = ctx.device();
        pipe_ = ctx.makePipeline(hud_mask_comp, hud_mask_comp_size, {CIS, CIS, SI, SB});
        mask_ = ctx.createImage(VK_FORMAT_R32_SFLOAT, ctx.renderExtent(),
                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        counts_ = dev.createBuffer(8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
        dev.immediate([&](VkCommandBuffer cmd) {
            VkClearColorValue zero{};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            dev.vt.CmdClearColorImage(cmd, mask_.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
            dev.vt.CmdFillBuffer(cmd, counts_.buffer, 0, 8, 0);
        });
        const uint32_t n = ctx.sourceCount();
        for (uint32_t prev = 0; prev < n; ++prev)
            for (uint32_t cur = 0; cur < n; ++cur) {
                VkDescriptorSet s = ctx.allocSet(pipe_.setLayout);
                ctx.writeImage(s, 0, CIS, ctx.sourceView(prev));
                ctx.writeImage(s, 1, CIS, ctx.sourceView(cur));
                ctx.writeImage(s, 2, SI, mask_.view);
                ctx.writeBuffer(s, 3, counts_.buffer);
                sets_.push_back(s);
            }
        ctx.setStaticMask(mask_.view);
    }
    void destroy(FgContext& ctx) override {
        DeviceData& dev = ctx.device();
        if (mask_.image) dev.destroyImage(mask_);
        dev.destroyBuffer(counts_);
        ctx.destroyPipeline(pipe_);
    }

    void frameBegin(FgContext&) override { ++frame_; }

    VkDescriptorSet synthesisInputs(FgContext& ctx, VkCommandBuffer cmd, uint32_t prev, uint32_t cur, uint32_t) override {
        if (updated_ == frame_) return VK_NULL_HANDLE;  // once per real frame
        updated_ = frame_;
        auto& vt = ctx.device().vt;
        const VkExtent2D e = ctx.renderExtent();
        slot_ ^= 1;
        vt.CmdFillBuffer(cmd, counts_.buffer, 4 * VkDeviceSize(slot_), 4, 0);
        ctx.barrier(cmd);
        HudMaskPC pc{{(int32_t)e.width, (int32_t)e.height}, (int32_t)slot_, (e.width * e.height) / 4};
        vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.pipeline);
        vt.CmdPushConstants(cmd, pipe_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.layout, 0, 1,
                                 &sets_[prev * ctx.sourceCount() + cur], 0, nullptr);
        vt.CmdDispatch(cmd, fg::divUp(e.width, 16), fg::divUp(e.height, 16), 1);
        ctx.barrier(cmd);
        return VK_NULL_HANDLE;  // the synthesis keeps its inputs; it reads the mask
    }

private:
    FgPipeline pipe_;
    AllocatedImage mask_;
    AllocatedBuffer counts_;
    std::vector<VkDescriptorSet> sets_;  // [prev * sources + cur]
    uint64_t frame_ = 0, updated_ = ~0ull;
    uint32_t slot_ = 0;
};

} // namespace

std::unique_ptr<FgPass> makeHudMaskPass() { return std::make_unique<HudMaskPass>(); }

} // namespace bdex
