// flow_confidence: the trust in the finest flow, per flow texel
// (shaders/flow_confidence.comp), after the flow and before the syntheses —
// bound to every synthesis (FgContext::setConfidenceMap) for
// confidence_fallback and debug=confidence, and counted (the share of
// uncertain texels in the statistics, the panel and --report).
#include "fg_pass.h"
#include "framegen_internal.h"
#include "shaders.h"

namespace bdex {
namespace {

struct ConfPC { int32_t fine[2]; int32_t luma[2]; int32_t block; int32_t slot; };

constexpr uint32_t kSlots = 20;  // FrameGen::kProfileSlots: one count pair per command slot

class FlowConfidencePass : public FgPass {
public:
    const char* name() const override { return "flow_confidence"; }

    void create(FgContext& ctx) override {
        const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                               SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        DeviceData& dev = ctx.device();
        pipe_ = ctx.makePipeline(flow_confidence_comp, flow_confidence_comp_size, {CIS, CIS, CIS, CIS, SI, SB});
        const FgFineLevel l0 = ctx.fineLevel();
        map_ = ctx.createImage(VK_FORMAT_R32_SFLOAT, l0.fine, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        counts_ = dev.createBuffer(kSlots * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        VK_CHECK(dev.vt.MapMemory(dev.device, counts_.memory, 0, VK_WHOLE_SIZE, 0, &mapped_));
        for (uint32_t parity = 0; parity < 2; ++parity) {
            VkDescriptorSet& s = sets_[parity];
            s = ctx.allocSet(pipe_.setLayout);
            ctx.writeImage(s, 0, CIS, l0.flow[0]);
            ctx.writeImage(s, 1, CIS, l0.flow[1]);
            ctx.writeImage(s, 2, CIS, l0.pyr[parity]);      // current frame
            ctx.writeImage(s, 3, CIS, l0.pyr[1 - parity]);  // previous frame
            ctx.writeImage(s, 4, SI, map_.view);
            ctx.writeBuffer(s, 5, counts_.buffer);
        }
        ctx.setConfidenceMap(map_.view);
    }
    void destroy(FgContext& ctx) override {
        DeviceData& dev = ctx.device();
        if (mapped_) dev.vt.UnmapMemory(dev.device, counts_.memory);
        mapped_ = nullptr;
        if (map_.image) dev.destroyImage(map_);
        dev.destroyBuffer(counts_);
        ctx.destroyPipeline(pipe_);
    }

    // The slot's previous submission completed: its counts are final.
    void slotBegin(FgContext& ctx, uint32_t slot) override {
        slot_ = slot;
        if (slot >= kSlots || !pending_[slot]) return;
        pending_[slot] = false;
        const auto* w = static_cast<const uint32_t*>(mapped_) + 2 * slot;
        if (w[1] > 0) ctx.reportConfidence(w[0], w[1]);
    }

    void fineFlow(FgContext& ctx, VkCommandBuffer cmd, uint32_t parity, const Config&) override {
        auto& vt = ctx.device().vt;
        const FgFineLevel l0 = ctx.fineLevel();
        const bool counted = slot_ < kSlots;
        if (counted) {
            vt.CmdFillBuffer(cmd, counts_.buffer, 8 * VkDeviceSize(slot_), 8, 0);
            ctx.barrier(cmd);
        }
        ConfPC pc{{(int32_t)l0.fine.width, (int32_t)l0.fine.height}, {(int32_t)l0.luma.width, (int32_t)l0.luma.height},
                  (int32_t)l0.block, counted ? (int32_t)slot_ : 0};
        vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.pipeline);
        vt.CmdPushConstants(cmd, pipe_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.layout, 0, 1, &sets_[parity & 1], 0, nullptr);
        vt.CmdDispatch(cmd, fg::divUp(l0.fine.width, 16), fg::divUp(l0.fine.height, 16), 1);
        ctx.barrier(cmd);
        if (counted) pending_[slot_] = true;
    }

private:
    FgPipeline pipe_;
    AllocatedImage map_;
    AllocatedBuffer counts_;
    void* mapped_ = nullptr;
    VkDescriptorSet sets_[2] = {};
    uint32_t slot_ = kSlots;
    bool pending_[kSlots] = {};
};

} // namespace

std::unique_ptr<FgPass> makeFlowConfidencePass() { return std::make_unique<FlowConfidencePass>(); }

} // namespace bdex
