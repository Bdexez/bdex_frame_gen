// FrameGen: the real frame's output stage — spatial upscaling (or what an
// optional pass provides, e.g. upscale_temporal), the HUD, the dedup
// replacement and CAS.
#include "framegen.h"
#include "framegen_internal.h"
#include "vk_util.h"

#include <algorithm>

namespace bdex {

using namespace fg;

void FrameGen::recordUpscaleReal(VkCommandBuffer cmd, uint32_t cur, int prev) {
    auto& vt = dev_.vt;
    // A pass may provide the frame to pack (upscale_temporal: its history).
    VkDescriptorSet set = dsUpscale_[cur];
    VkExtent2D src = extent_;
    for (auto& p : passes_) p->realOutput(*this, cmd, cur, set, src);
    vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, upscale_.pipeline);
    UpscalePC pc{{(int32_t)displayExtent_.width, (int32_t)displayExtent_.height},
                 {(int32_t)src.width, (int32_t)src.height},
                 enc_.encoding, std::clamp((*cfgp_).upscaleFilter, 0, 2)};
    vt.CmdPushConstants(cmd, upscale_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, upscale_.layout, 0, 1, &set, 0, nullptr);
    vt.CmdDispatch(cmd, divUp(displayExtent_.width, 16), divUp(displayExtent_.height, 16), 1);
    // Order the CAS pass / the worker's copy after this write.
    memoryBarrier(cmd);
    // dedup + extrapolation: a repeated frame shows the game's state from its
    // first appearance; replace it with a frame extrapolated to now along
    // the kept flow (state slot 7, the real frame's). Zero workgroups unless it is a repeat.
    if (dupReal_ && timingActive_ && prev >= 0) {
        const uint32_t N = static_cast<uint32_t>(sources_.size());
        recordSynthesis(cmd, dsInterpReal_[uint32_t(prev) * N + cur], 1.5f, kMaxFrames, true);
    }
    if (sharpen_) recordCas(cmd, dsCasReal_);  // sharpReal_ -> outReal_
    for (auto& p : passes_) p->afterOutput(*this, cmd, -1);  // (the fps HUD)
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
