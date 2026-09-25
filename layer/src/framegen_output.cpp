// FrameGen: the real frame's output stage — spatial or temporal
// (upscale_temporal) upscaling, the HUD, the dedup replacement and CAS.
#include "framegen.h"
#include "framegen_internal.h"
#include "vk_util.h"

#include <algorithm>

namespace bdex {

using namespace fg;

void FrameGen::recordTemporalUpscale(VkCommandBuffer cmd, uint32_t cur) {
    if (taaFrame_ == frame_) return;  // once per frame (synthesis or real output first)
    auto& vt = dev_.vt;
    const Config cfg = effectiveConfig();
    taaPrevValid_ = taaFrame_ + 1 == frame_;
    const bool history = taaPrevValid_ && flowFrame_ == frame_;
    taaH_ = taaFrame_ == ~0ull ? 0 : 1 - taaH_;
    taaFrame_ = frame_;
    const Level& fine = levels_v_[0];
    const float sx = float(displayExtent_.width) / float(extent_.width);
    const float sy = float(displayExtent_.height) / float(extent_.height);
    const float blk = float(fine.refined ? kFineBlock : kBlockSize);
    TaaPC pc{{(int32_t)displayExtent_.width, (int32_t)displayExtent_.height},
             {(int32_t)extent_.width, (int32_t)extent_.height},
             {1.f / (fine.fine.width * blk * flowScale_ * sx), 1.f / (fine.fine.height * blk * flowScale_ * sy)},
             flowScale_ * sx, cfg.upscaleTemporal, cfg.sceneCutLow, cfg.sceneCutHigh,
             std::clamp(cfg.upscaleFilter, 0, 2), history ? 1 : 0, timingActive_ && dedup_ ? 1 : 0};
    vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, taaPass_.pipeline);
    vt.CmdPushConstants(cmd, taaPass_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, taaPass_.layout, 0, 1, &dsTaa_[cur * 2 + taaH_], 0, nullptr);
    vt.CmdDispatch(cmd, divUp(displayExtent_.width, 16), divUp(displayExtent_.height, 16), 1);
    memoryBarrier(cmd);
}

void FrameGen::recordUpscaleReal(VkCommandBuffer cmd, uint32_t cur, int prev) {
    auto& vt = dev_.vt;
    // With upscale_temporal the real frame is the accumulated history, only
    // packed (and given the HUD) at 1:1.
    if (taa_) recordTemporalUpscale(cmd, cur);
    const VkExtent2D src = taa_ ? displayExtent_ : extent_;
    vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, upscale_.pipeline);
    UpscalePC pc{{(int32_t)displayExtent_.width, (int32_t)displayExtent_.height},
                 {(int32_t)src.width, (int32_t)src.height},
                 enc_.encoding, std::clamp((*cfgp_).upscaleFilter, 0, 2),
                 hudMode_, hudGame_, hudOut_, hudLow1_, hudLow01_, hudLatency_};
    vt.CmdPushConstants(cmd, upscale_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, upscale_.layout, 0, 1,
                             taa_ ? &dsUpscaleTaa_[taaH_] : &dsUpscale_[cur], 0, nullptr);
    vt.CmdDispatch(cmd, divUp(displayExtent_.width, 16), divUp(displayExtent_.height, 16), 1);
    // Order the CAS pass / the worker's copy after this write.
    memoryBarrier(cmd);
    // dedup + extrapolation: a repeated frame shows the game's state from its
    // first appearance; replace it with a frame extrapolated to now along
    // the kept flow (state slot 3). Zero workgroups unless it is a repeat.
    if (dupReal_ && timingActive_ && prev >= 0) {
        const uint32_t N = static_cast<uint32_t>(sources_.size());
        recordSynthesis(cmd, dsInterpReal_[uint32_t(prev) * N + cur], 1.5f, 4, true);
    }
    if (sharpen_) recordCas(cmd, dsCasReal_);  // sharpReal_ -> outReal_
}

void FrameGen::recordCas(VkCommandBuffer cmd, VkDescriptorSet set) {
    auto& vt = dev_.vt;
    vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cas_.pipeline);
    CasPC pc{{(int32_t)displayExtent_.width, (int32_t)displayExtent_.height}, enc_.encoding, (*cfgp_).sharpness};
    vt.CmdPushConstants(cmd, cas_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cas_.layout, 0, 1, &set, 0, nullptr);
    vt.CmdDispatch(cmd, divUp(displayExtent_.width, 16), divUp(displayExtent_.height, 16), 1);
    memoryBarrier(cmd);
}

} // namespace bdex
