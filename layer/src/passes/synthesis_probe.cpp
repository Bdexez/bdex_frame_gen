#include "synthesis_probe.h"
#include "framegen_internal.h"
#include "shaders.h"

namespace bdex {
namespace {

struct ComposePC { int32_t size[2]; float extent[2]; };
struct ComparePC { int32_t size[2]; int32_t slot; int32_t step; float blend; };

} // namespace

void SynthesisProbe::create(FgContext& ctx, bool extrapolate, VkBuffer counters) {
    const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    extrapolate_ = extrapolate;
    const FgFineLevel l0 = ctx.fineLevel();
    const uint32_t n = ctx.sourceCount();
    synth_ = ctx.makeLinearSynthesisPipeline();
    compare_ = ctx.makePipeline(probe_compare_comp, probe_compare_comp_size, {CIS, CIS, CIS, CIS, SB});
    copyA_ = ctx.createImage(ctx.sourceFormat(), ctx.renderExtent(), VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    probe_ = ctx.createImage(VK_FORMAT_R16G16B16A16_SFLOAT, ctx.displayExtent(),
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    VkImageView fwd = l0.flow[0], bwd = l0.flow[1];
    if (!extrapolate_) {
        compose_ = ctx.makePipeline(flow_compose_comp, flow_compose_comp_size, {CIS, CIS, CIS, CIS, SI, SI});
        const VkImageUsageFlags flowUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        fwdAB_ = ctx.createImage(VK_FORMAT_R32G32_SFLOAT, l0.fine, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        fwdAC_ = ctx.createImage(VK_FORMAT_R32G32_SFLOAT, l0.fine, flowUsage);
        bwdCA_ = ctx.createImage(VK_FORMAT_R32G32_SFLOAT, l0.fine, flowUsage);
        composeSet_ = ctx.allocSet(compose_.setLayout);
        ctx.writeImage(composeSet_, 0, CIS, fwdAB_.view);
        ctx.writeImage(composeSet_, 1, CIS, l0.flow[0]);
        ctx.writeImage(composeSet_, 2, CIS, l0.flow[1]);
        ctx.writeImage(composeSet_, 3, CIS, ctx.previousBackwardFlow());
        ctx.writeImage(composeSet_, 4, SI, fwdAC_.view);
        ctx.writeImage(composeSet_, 5, SI, bwdCA_.view);
        fwd = fwdAC_.view;
        bwd = bwdCA_.view;
    }
    for (uint32_t i = 0; i < n; ++i)
        synthSets_.push_back(ctx.synthesisSetFor(synth_.setLayout, copyA_.view, ctx.sourceView(i), fwd, bwd, probe_.view));
    for (uint32_t prev = 0; prev < n; ++prev)
        for (uint32_t cur = 0; cur < n; ++cur) {
            // Interpolation: truth B = prev, naive = mix(A, C = cur); extrapolation:
            // truth C = cur, naive = hold B = prev.
            VkDescriptorSet s = ctx.allocSet(compare_.setLayout);
            ctx.writeImage(s, 0, CIS, probe_.view);
            ctx.writeImage(s, 1, CIS, ctx.sourceView(extrapolate_ ? cur : prev));
            ctx.writeImage(s, 2, CIS, extrapolate_ ? ctx.sourceView(prev) : copyA_.view);
            ctx.writeImage(s, 3, CIS, ctx.sourceView(extrapolate_ ? prev : cur));
            ctx.writeBuffer(s, 4, counters);
            compareSets_.push_back(s);
        }
}

void SynthesisProbe::destroy(FgContext& ctx) {
    DeviceData& dev = ctx.device();
    for (AllocatedImage* img : {&copyA_, &fwdAB_, &fwdAC_, &bwdCA_, &probe_})
        if (img->image) dev.destroyImage(*img);
    ctx.destroyPipeline(synth_);
    ctx.destroyPipeline(compose_);
    ctx.destroyPipeline(compare_);
}

void SynthesisProbe::copyImage(FgContext& ctx, VkCommandBuffer cmd, VkImage src, VkImage dst, VkExtent2D extent) {
    ctx.barrier(cmd);  // after the reads of dst earlier in the command buffer
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {extent.width, extent.height, 1};
    ctx.device().vt.CmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    ctx.barrier(cmd);
}

void SynthesisProbe::flowBegin(FgContext& ctx, VkCommandBuffer cmd, bool active) {
    if (!active || !ctx.flowLastFrame()) return;  // the flow images must hold A->B
    if (!extrapolate_) {
        // Keep A->B before recordFlow replaces it with B->C (B->A is kept by
        // the engine as the previous backward flow).
        const FgFineLevel l0 = ctx.fineLevel();
        copyImage(ctx, cmd, l0.flowImage[0], fwdAB_.image, l0.fine);
        fwdFrame_ = frame_;
        return;
    }
    // Predict C from A (copy) and B (last frame's cur, still acquired as prev).
    if (copyFrame_ + 1 != frame_ || lastFrame_ + 1 != frame_) return;
    ctx.recordSynthesisWith(cmd, synth_, synthSets_[lastCur_], 2.f, ctx.effectiveConfig());
    probeFrame_ = frame_;
}

bool SynthesisProbe::measure(FgContext& ctx, VkCommandBuffer cmd, uint32_t prev, uint32_t cur, uint32_t slot,
                             bool active) {
    if (lastFrame_ == frame_) return false;  // once per frame
    auto& vt = ctx.device().vt;
    const uint32_t n = ctx.sourceCount();
    bool probed = false;
    if (active) {
        bool ready = false;
        if (!extrapolate_ && fwdFrame_ == frame_ && copyFrame_ + 1 == frame_) {
            // Rebuild B from A and C.
            const FgFineLevel l0 = ctx.fineLevel();
            vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compose_.pipeline);
            ComposePC cpc{{(int32_t)l0.fine.width, (int32_t)l0.fine.height},
                          {float(l0.fine.width * l0.block), float(l0.fine.height * l0.block)}};
            vt.CmdPushConstants(cmd, compose_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cpc), &cpc);
            vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compose_.layout, 0, 1, &composeSet_, 0, nullptr);
            vt.CmdDispatch(cmd, fg::divUp(l0.fine.width, 16), fg::divUp(l0.fine.height, 16), 1);
            ctx.barrier(cmd);
            ctx.recordSynthesisWith(cmd, synth_, synthSets_[cur], 0.5f, ctx.effectiveConfig());
            ready = true;
        } else if (extrapolate_ && probeFrame_ == frame_) {
            ready = true;  // synthesised at flowBegin
        }
        if (ready && slot < 16) {
            const VkExtent2D d = ctx.displayExtent();
            uint32_t step = 2;
            while (uint64_t(fg::divUp(d.width, step)) * fg::divUp(d.height, step) > (1u << 18)) ++step;
            vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compare_.pipeline);
            ComparePC pc{{(int32_t)d.width, (int32_t)d.height}, (int32_t)slot, (int32_t)step, extrapolate_ ? 1.f : 0.5f};
            vt.CmdPushConstants(cmd, compare_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compare_.layout, 0, 1,
                                     &compareSets_[prev * n + cur], 0, nullptr);
            vt.CmdDispatch(cmd, fg::divUp(fg::divUp(d.width, step), 16), fg::divUp(fg::divUp(d.height, step), 16), 1);
            ctx.barrier(cmd);
            probed = true;
        }
        // B becomes the next frame's A.
        copyImage(ctx, cmd, ctx.sourceImage(prev), copyA_.image, ctx.renderExtent());
        copyFrame_ = frame_;
    }
    lastFrame_ = frame_;
    lastCur_ = cur;
    return probed;
}

} // namespace bdex
