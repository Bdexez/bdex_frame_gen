#include "framegen.h"
#include "framegen_internal.h"
#include "log.h"
#include "shaders.h"
#include "vk_util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace bdex {

using namespace fg;


FrameGen::FrameGen(DeviceData& dev, VkFormat format, VkExtent2D renderExtent, VkExtent2D displayExtent, uint32_t outputs,
                   const std::vector<VkImage>& sources, VkColorSpaceKHR colorSpace, int flowScaleFactor,
                   const Config* config)
    : dev_(dev), cfgp_(config ? config : &dev.config), format_(format), extent_(renderExtent),
      displayExtent_(displayExtent), outputs_(outputs),
      flowScaleFactor_(std::clamp(flowScaleFactor, 1, 4)) {
    upscaling_ = displayExtent_.width != extent_.width || displayExtent_.height != extent_.height;
    hudMode_ = (*cfgp_).overlayMode;
    dedup_ = outputs_ > 0 && (*cfgp_).dedup;
    chroma_ = outputs_ > 0 && (*cfgp_).chromaWeight > 0.f;
    dupReal_ = dedup_ && (*cfgp_).extrapolate;
    // The real frame is resampled into an internal packed image when upscaling,
    // when the HUD must be drawn on it, or when a repeated frame may have to
    // be replaced by an extrapolated one (otherwise it is copied straight).
    packReal_ = upscaling_ || hudMode_ > 0 || dupReal_;
    passes_ = createOptionalPasses(*cfgp_, dev, outputs_, upscaling_);
    // The CAS pass needs a linear rgba16f intermediate it can both write and
    // sample with filtering; skip sharpening if the driver cannot provide it.
    sharpen_ = upscaling_ && (*cfgp_).sharpness > 0.f &&
               dev.formatSupports(VK_FORMAT_R16G16B16A16_SFLOAT,
                                  VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    enc_ = encodingForFormat(format, colorSpace);
    if (!enc_.supported) throw VkError(VK_ERROR_FORMAT_NOT_SUPPORTED, "swapchain format not supported by frame generation");
    dumpBuf_.resize(outputs_ + 1);
    dumpMapped_.resize(outputs_ + 1, nullptr);
    for (VkImage img : sources) sources_.push_back({img, VK_NULL_HANDLE});
    if (outputs_ == 0 && !packReal_) return;  // pass-through: only recordCopySource is used
    try {
        for (auto& src : sources_) {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = src.image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = format_;
            vi.subresourceRange = colorRange();
            VK_CHECK(dev_.vt.CreateImageView(dev_.device, &vi, nullptr, &src.view));
        }
        createPipelines();
        createResources();
        createDescriptors();
        for (auto& p : passes_) p->create(*this);
    } catch (...) {
        destroyAll();
        throw;
    }
    if (outputs_ > 0)
        BDEX_INFO("frame generation ready: render %ux%u, %d flow levels (finest %ux%u blocks, scale %.0f), output %s%s",
                  extent_.width, extent_.height, levels_, levels_v_[0].blocks.width, levels_v_[0].blocks.height,
                  flowScale_, enc_.is64 ? "rg32ui" : "r32ui",
                  upscaling_ ? " (upscaled)" : "");
    if (upscaling_) {
        static const char* filt[] = {"bilinear", "Catmull-Rom", "Lanczos-2"};
        BDEX_INFO("upscaling %ux%u -> %ux%u (%.0f%%, %s%s)", extent_.width, extent_.height, displayExtent_.width,
                  displayExtent_.height, 100.0 * extent_.width / displayExtent_.width,
                  filt[std::clamp((*cfgp_).upscaleFilter, 0, 2)],
                  sharpen_ ? ", CAS sharpening" : "");
    }
}

FrameGen::~FrameGen() { destroyAll(); }

FrameGen::Pass FrameGen::makePass(const uint32_t* spv, size_t spvSize, const std::vector<VkDescriptorType>& bindings) {
    Pass p;
    std::vector<VkDescriptorSetLayoutBinding> b(bindings.size());
    for (uint32_t i = 0; i < bindings.size(); ++i) {
        b[i] = {i, bindings[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = static_cast<uint32_t>(b.size());
    li.pBindings = b.data();
    VK_CHECK(dev_.vt.CreateDescriptorSetLayout(dev_.device, &li, nullptr, &p.setLayout));
    planner_.registerLayout(p.setLayout, bindings);

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, kPushSize};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &p.setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VK_CHECK(dev_.vt.CreatePipelineLayout(dev_.device, &pli, nullptr, &p.layout));

    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = spvSize;
    smi.pCode = spv;
    VkShaderModule mod = VK_NULL_HANDLE;
    VK_CHECK(dev_.vt.CreateShaderModule(dev_.device, &smi, nullptr, &mod));

    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
    ci.layout = p.layout;
    VkResult r = dev_.vt.CreateComputePipelines(dev_.device, VK_NULL_HANDLE, 1, &ci, nullptr, &p.pipeline);
    dev_.vt.DestroyShaderModule(dev_.device, mod, nullptr);
    if (r < 0) throw VkError(r, "vkCreateComputePipelines");
    return p;
}

void FrameGen::createPipelines() {
    using T = VkDescriptorType;
    const T CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    const T SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    const T SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    // 16-bit luma halves the bandwidth of the matching passes; it is not a
    // mandatory storage format so fall back to 32-bit when unsupported.
    const VkFormatFeatureFlags lumaFeatures = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    lumaFormat_ = dev_.formatSupports(VK_FORMAT_R16_SFLOAT, lumaFeatures) ? VK_FORMAT_R16_SFLOAT : VK_FORMAT_R32_SFLOAT;
    if (outputs_ > 0) {
        if (lumaFormat_ == VK_FORMAT_R16_SFLOAT) {
            downsample_ = makePass(downsample_comp_f16, downsample_comp_f16_size, {CIS, SI});
            blockMatch_ = makePass(block_match_comp_f16, block_match_comp_f16_size, {SI, SI, CIS, SI, SI, CIS, CIS, CIS});
            refine_ = makePass(flow_refine_comp_f16, flow_refine_comp_f16_size, {SI, SI, SI, SI, SI});
        } else {
            downsample_ = makePass(downsample_comp_f32, downsample_comp_f32_size, {CIS, SI});
            blockMatch_ = makePass(block_match_comp_f32, block_match_comp_f32_size, {SI, SI, CIS, SI, SI, CIS, CIS, CIS});
            refine_ = makePass(flow_refine_comp_f32, flow_refine_comp_f32_size, {SI, SI, SI, SI, SI});
        }
        smooth_ = makePass(flow_smooth_comp, flow_smooth_comp_size, {SI, SI});
        reduce_ = makePass(reduce_cost_comp, reduce_cost_comp_size, {SI, SB});
        // With sharpening, the interpolation writes a linear rgba16f image that
        // the CAS pass then sharpens and packs; otherwise it packs directly.
        if (sharpen_)       interp_ = makePass(interpolate_comp_sharp, interpolate_comp_sharp_size, {CIS, CIS, CIS, CIS, SI, SB, SI, CIS, CIS});
        else if (enc_.is64) interp_ = makePass(interpolate_comp_u64, interpolate_comp_u64_size, {CIS, CIS, CIS, CIS, SI, SB, SI, CIS, CIS});
        else                interp_ = makePass(interpolate_comp_u32, interpolate_comp_u32_size, {CIS, CIS, CIS, CIS, SI, SB, SI, CIS, CIS});
        splat_ = makePass(splat_comp, splat_comp_size, {CIS, CIS, SI, SB, CIS});
        frameState_ = makePass(frame_state_comp, frame_state_comp_size, {CIS, CIS, SB});
        if (chroma_) downChroma_ = makePass(downsample_chroma_comp, downsample_chroma_comp_size, {CIS, SI});
    }
    if (packReal_) {
        if (sharpen_)       upscale_ = makePass(upscale_comp_sharp, upscale_comp_sharp_size, {CIS, SI});
        else if (enc_.is64) upscale_ = makePass(upscale_comp_u64, upscale_comp_u64_size, {CIS, SI});
        else                upscale_ = makePass(upscale_comp_u32, upscale_comp_u32_size, {CIS, SI});
    }
    if (sharpen_) {
        if (enc_.is64) cas_ = makePass(cas_comp_u64, cas_comp_u64_size, {CIS, SI});
        else           cas_ = makePass(cas_comp_u32, cas_comp_u32_size, {CIS, SI});
    }
}

void FrameGen::createResources() {
    const Config& cfg = (*cfgp_);
    const VkImageUsageFlags storageSampled = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    if (!dev_.formatSupports(format_, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
        throw VkError(VK_ERROR_FORMAT_NOT_SUPPORTED, "swapchain format cannot be sampled");
    if (!dev_.formatSupports(enc_.storageFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
        throw VkError(VK_ERROR_FORMAT_NOT_SUPPORTED, "output storage format unsupported");

    // Pyramid: level 0 at full or half resolution, then halving. Stop early
    // when a level would have fewer than 2x2 blocks.
    const int fs = std::min(4, cfg.flowScaleFor(extent_.width, extent_.height) * flowScaleFactor_);
    VkExtent2D size = {divUp(extent_.width, fs), divUp(extent_.height, fs)};
    flowScale_ = float(fs);
    for (int l = 0; outputs_ > 0 && l < cfg.levels; ++l) {
        Level lv;
        lv.size = size;
        lv.blocks = {divUp(size.width, kBlockSize), divUp(size.height, kBlockSize)};
        lv.fine = {divUp(size.width, kFineBlock), divUp(size.height, kFineBlock)};
        if (l > 0 && (lv.blocks.width < 2 || lv.blocks.height < 2)) break;
        for (int p = 0; p < 2; ++p) lv.pyr[p] = dev_.createImage(lumaFormat_, lv.size, storageSampled, true);
        if (chroma_)
            for (int p = 0; p < 2; ++p) lv.chroma[p] = dev_.createImage(VK_FORMAT_R32G32_SFLOAT, lv.size, storageSampled, true);
        for (int d = 0; d < 2; ++d) {
            lv.flowRaw[d] = dev_.createImage(VK_FORMAT_R32G32_SFLOAT, lv.blocks, storageSampled, true);
            // The finest backward flow is copied for extrapolate_accel
            // (flowFine, which aliases flow when the level is not refined).
            const VkImageUsageFlags flowUsage = storageSampled | (l == 0 ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
            lv.flow[d] = dev_.createImage(VK_FORMAT_R32G32_SFLOAT, lv.blocks, flowUsage, true);
            // Refinement runs on the finest level only unless refine_all is
            // set: coarser levels just predict the search centre, where the
            // smoothed flow is good enough.
            const bool refined = cfg.refine && (l == 0 || cfg.refineAll);
            lv.refined = refined;
            if (refined) lv.flowFine[d] = dev_.createImage(VK_FORMAT_R32G32_SFLOAT, lv.fine, flowUsage, true);
            else { lv.flowFine[d] = lv.flow[d]; lv.fine = lv.blocks; }
            lv.cost[d] = dev_.createImage(VK_FORMAT_R32_SFLOAT, lv.blocks, storageSampled, true);
        }
        levels_v_.push_back(lv);
        size = {divUp(size.width, 2), divUp(size.height, 2)};
    }
    levels_ = static_cast<int>(levels_v_.size());

    const VkImageUsageFlags outUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    for (uint32_t i = 0; i < outputs_; ++i)
        out_.push_back(dev_.createImage(enc_.storageFormat, displayExtent_, outUsage, true));
    if (packReal_) outReal_ = dev_.createImage(enc_.storageFormat, displayExtent_, outUsage, true);
    if (sharpen_) {
        const VkImageUsageFlags sharpUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        for (uint32_t i = 0; i < outputs_; ++i)
            sharp_.push_back(dev_.createImage(VK_FORMAT_R16G16B16A16_SFLOAT, displayExtent_, sharpUsage, true));
        sharpReal_ = dev_.createImage(VK_FORMAT_R16G16B16A16_SFLOAT, displayExtent_, sharpUsage, true);
    }
    if (outputs_ > 0) {
        stateBuf_ = dev_.createBuffer(kStateSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
        stateReadback_ = dev_.createBuffer(kStateHead, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        VK_CHECK(dev_.vt.MapMemory(dev_.device, stateReadback_.memory, 0, VK_WHOLE_SIZE, 0, &stateMapped_));
        // The dispatches the state pass may gate, in a fixed order: per flow
        // level l, block matching (3l), smoothing (3l+1) and refinement
        // (3l+2); then the scene-cut reduction; then the extrapolated
        // replacement of a repeated real frame, which runs only on repeats.
        auto gate = [&](uint32_t x, uint32_t y, bool onDuplicates) {
            gateFull_.insert(gateFull_.end(), {x, y, 1u, onDuplicates ? 1u : 0u});
            return static_cast<uint32_t>(gateFull_.size() / 4 - 1);
        };
        for (const Level& lv : levels_v_) {
            gate(lv.blocks.width, lv.blocks.height, false);
            gate(divUp(lv.blocks.width, 16), divUp(lv.blocks.height, 16), false);
            gate(divUp(lv.fine.width, 16), divUp(lv.fine.height, 16), false);
        }
        gateReduce_ = gate(1 + kCutTiles * kCutTiles, 1, false);  // global + local tiles (idle when off)
        gateDupReal_ = gate(divUp(displayExtent_.width, 16), divUp(displayExtent_.height, 16), true);
        if (gateFull_.size() / 4 > kGateEntries) throw VkError(VK_ERROR_INITIALIZATION_FAILED, "too many gated dispatches");
        // The splat key packs the source index in 23 bits (4K fits).
        splatting_ = cfg.extrapolate && cfg.extrapolateSplat &&
                     uint64_t(displayExtent_.width) * displayExtent_.height <= (1ull << 23);
        splatMap_ = dev_.createImage(VK_FORMAT_R32_UINT, splatting_ ? displayExtent_ : VkExtent2D{1, 1},
                                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, true);
        // Small (finest flow resolution); created in either mode so that
        // extrapolate_accel can be toggled live.
        prevBwd_ = dev_.createImage(VK_FORMAT_R32G32_SFLOAT, levels_v_[0].fine,
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, true);
        if (!chroma_) chromaDummy_ = dev_.createImage(VK_FORMAT_R32G32_SFLOAT, {1, 1}, VK_IMAGE_USAGE_SAMPLED_BIT, true);
        // Bound as the synthesis's static mask until a pass provides one (hud_mask).
        maskDummy_ = dev_.createImage(VK_FORMAT_R32_SFLOAT, {1, 1}, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, true);
        staticMask_ = maskDummy_.view;
    }

    // (Offline and capture tools fill no queue families: assume timestamps.)
    if ((cfg.profile || cfg.gpuBudget > 0.f) && (dev_.families.empty() || dev_.timestampsUsable())) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = kProfileSlots * (StageCount + 1);
        VK_CHECK(dev_.vt.CreateQueryPool(dev_.device, &qi, nullptr, &queryPool_));
        timestampPeriodNs_ = dev_.props.limits.timestampPeriod;
    }

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 1.f;
    VK_CHECK(dev_.vt.CreateSampler(dev_.device, &si, nullptr, &sampler_));

    // Everything internal lives in GENERAL layout for its whole life.
    dev_.immediate([&](VkCommandBuffer cmd) {
        std::vector<VkImageMemoryBarrier> barriers;
        auto add = [&](const AllocatedImage& img) {
            barriers.push_back(imageBarrier(img.image, 0, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL));
        };
        for (auto& lv : levels_v_) {
            for (auto& i : lv.pyr) add(i);
            if (chroma_) for (auto& i : lv.chroma) add(i);
            for (auto& i : lv.flowRaw) add(i);
            for (auto& i : lv.flow) add(i);
            if (lv.refined) for (auto& i : lv.flowFine) add(i);
            for (auto& i : lv.cost) add(i);
        }
        for (auto& o : out_) add(o);
        if (splatMap_.image) add(splatMap_);
        if (prevBwd_.image) add(prevBwd_);
        if (chromaDummy_.image) add(chromaDummy_);
        if (maskDummy_.image) add(maskDummy_);
        if (packReal_) add(outReal_);
        for (auto& s : sharp_) add(s);
        if (sharpen_) add(sharpReal_);
        dev_.vt.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                   nullptr, static_cast<uint32_t>(barriers.size()), barriers.data());
        if (maskDummy_.image) {
            VkClearColorValue zero{};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            dev_.vt.CmdClearColorImage(cmd, maskDummy_.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        }
        if (stateBuf_.buffer) {
            dev_.vt.CmdFillBuffer(cmd, stateBuf_.buffer, 0, VK_WHOLE_SIZE, 0);
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            dev_.vt.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
                                       nullptr, 0, nullptr);
            dev_.vt.CmdUpdateBuffer(cmd, stateBuf_.buffer, kStateGateFull, gateFull_.size() * sizeof(uint32_t),
                                    gateFull_.data());
        }
    });
}

// Sets come from pools opened on demand: a pass declares its bindings
// (makePass) and allocates, nothing counts the totals by hand.
VkDescriptorSet FrameGen::allocSet(VkDescriptorSetLayout layout) {
    std::vector<VkDescriptorPoolSize> sizes;
    uint32_t maxSets = 0;
    if (planner_.plan(layout, sizes, maxSets)) {
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = maxSets;
        pi.poolSizeCount = static_cast<uint32_t>(sizes.size());
        pi.pPoolSizes = sizes.data();
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VK_CHECK(dev_.vt.CreateDescriptorPool(dev_.device, &pi, nullptr, &pool));
        pools_.push_back(pool);
    }
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pools_.back();
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VK_CHECK(dev_.vt.AllocateDescriptorSets(dev_.device, &ai, &set));
    return set;
}

void FrameGen::writeImage(VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkImageView view) {
    VkDescriptorImageInfo ii{sampler_, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = type;
    w.pImageInfo = &ii;
    dev_.vt.UpdateDescriptorSets(dev_.device, 1, &w, 0, nullptr);
}

void FrameGen::writeBuffer(VkDescriptorSet set, uint32_t binding, VkBuffer buffer) {
    VkDescriptorBufferInfo bi{buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &bi;
    dev_.vt.UpdateDescriptorSets(dev_.device, 1, &w, 0, nullptr);
}

void FrameGen::createDescriptors() {
    const uint32_t L = static_cast<uint32_t>(levels_);
    const uint32_t O = outputs_;
    const uint32_t N = static_cast<uint32_t>(sources_.size());
    const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    const VkDescriptorType SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

    if (packReal_) {
        for (uint32_t n = 0; n < N; ++n) {
            VkDescriptorSet s = allocSet(upscale_.setLayout);
            writeImage(s, 0, CIS, sources_[n].view);
            writeImage(s, 1, SI, (sharpen_ ? sharpReal_ : outReal_).view);
            dsUpscale_.push_back(s);
        }
    }
    if (sharpen_) {
        for (uint32_t o = 0; o < O; ++o) {
            VkDescriptorSet s = allocSet(cas_.setLayout);
            writeImage(s, 0, CIS, sharp_[o].view);
            writeImage(s, 1, SI, out_[o].view);
            dsCas_.push_back(s);
        }
        dsCasReal_ = allocSet(cas_.setLayout);
        writeImage(dsCasReal_, 0, CIS, sharpReal_.view);
        writeImage(dsCasReal_, 1, SI, outReal_.view);
    }
    if (O == 0) return;  // pure upscaling: no flow / interpolation descriptors

    for (uint32_t parity = 0; parity < 2; ++parity) {
        for (uint32_t n = 0; n < N; ++n) {
            VkDescriptorSet s = allocSet(downsample_.setLayout);
            writeImage(s, 0, CIS, sources_[n].view);
            writeImage(s, 1, SI, levels_v_[0].pyr[parity].view);
            dsDownSrc_[parity].push_back(s);
        }
        for (uint32_t l = 0; l < L; ++l) {
            VkDescriptorSet s = allocSet(downsample_.setLayout);
            writeImage(s, 0, CIS, l == 0 ? sources_[0].view : levels_v_[l - 1].pyr[parity].view);  // level 0 unused
            writeImage(s, 1, SI, levels_v_[l].pyr[parity].view);
            dsDown_[parity].push_back(s);
        }
        if (chroma_) {
            for (uint32_t n = 0; n < N; ++n) {
                VkDescriptorSet s = allocSet(downChroma_.setLayout);
                writeImage(s, 0, CIS, sources_[n].view);
                writeImage(s, 1, SI, levels_v_[0].chroma[parity].view);
                dsChromaSrc_[parity].push_back(s);
            }
            for (uint32_t l = 0; l < L; ++l) {
                VkDescriptorSet s = allocSet(downChroma_.setLayout);
                writeImage(s, 0, CIS, (l == 0 ? levels_v_[0] : levels_v_[l - 1]).chroma[parity].view);  // level 0 unused
                writeImage(s, 1, SI, levels_v_[l].chroma[parity].view);
                dsChroma_[parity].push_back(s);
            }
        }
        const uint32_t cur = parity, prev = 1 - parity;
        for (uint32_t dir = 0; dir < 2; ++dir) {
            for (uint32_t l = 0; l < L; ++l) {
                Level& lv = levels_v_[l];
                VkDescriptorSet s = allocSet(blockMatch_.setLayout);
                // dir 0 = forward: reference is the previous frame, target the current one.
                writeImage(s, 0, SI, lv.pyr[dir == 0 ? prev : cur].view);
                writeImage(s, 1, SI, lv.pyr[dir == 0 ? cur : prev].view);
                writeImage(s, 2, CIS, (l + 1 < L ? levels_v_[l + 1].flowFine[dir] : lv.flowFine[dir]).view);
                writeImage(s, 3, SI, lv.flowRaw[dir].view);
                writeImage(s, 4, SI, lv.cost[dir].view);
                // The previous pair's final backward flow (indexed by positions in
                // the current reference frame), read before this frame overwrites it.
                writeImage(s, 5, CIS, lv.flowFine[1].view);
                writeImage(s, 6, CIS, chroma_ ? lv.chroma[dir == 0 ? prev : cur].view : chromaDummy_.view);
                writeImage(s, 7, CIS, chroma_ ? lv.chroma[dir == 0 ? cur : prev].view : chromaDummy_.view);
                dsMatch_[parity][dir].push_back(s);

                VkDescriptorSet rs = allocSet(refine_.setLayout);
                writeImage(rs, 0, SI, lv.pyr[dir == 0 ? prev : cur].view);
                writeImage(rs, 1, SI, lv.pyr[dir == 0 ? cur : prev].view);
                writeImage(rs, 2, SI, lv.flowRaw[dir].view);
                writeImage(rs, 3, SI, lv.flow[dir].view);
                writeImage(rs, 4, SI, lv.flowFine[dir].view);
                dsRefine_[parity][dir].push_back(rs);
            }
        }
    }
    for (uint32_t dir = 0; dir < 2; ++dir) {
        for (uint32_t l = 0; l < L; ++l) {
            VkDescriptorSet s = allocSet(smooth_.setLayout);
            writeImage(s, 0, SI, levels_v_[l].flowRaw[dir].view);
            writeImage(s, 1, SI, levels_v_[l].flow[dir].view);
            dsSmooth_[dir].push_back(s);
        }
    }
    dsReduce_ = allocSet(reduce_.setLayout);
    writeImage(dsReduce_, 0, SI, levels_v_[0].cost[0].view);
    writeBuffer(dsReduce_, 1, stateBuf_.buffer);
    dsSplat_ = allocSet(splat_.setLayout);
    writeImage(dsSplat_, 0, CIS, levels_v_[0].flowFine[0].view);
    writeImage(dsSplat_, 1, CIS, levels_v_[0].flowFine[1].view);
    writeImage(dsSplat_, 2, SI, splatMap_.view);
    writeBuffer(dsSplat_, 3, stateBuf_.buffer);
    writeImage(dsSplat_, 4, CIS, prevBwd_.view);
    dsState_ = allocSet(frameState_.setLayout);
    writeImage(dsState_, 0, CIS, levels_v_[0].pyr[0].view);
    writeImage(dsState_, 1, CIS, levels_v_[0].pyr[1].view);
    writeBuffer(dsState_, 2, stateBuf_.buffer);

    for (uint32_t prev = 0; prev < N; ++prev) {
        for (uint32_t cur = 0; cur < N; ++cur) {
            for (uint32_t o = 0; o < O; ++o) {
                VkDescriptorSet s = allocSet(interp_.setLayout);
                writeImage(s, 0, CIS, sources_[prev].view);
                writeImage(s, 1, CIS, sources_[cur].view);
                writeImage(s, 2, CIS, levels_v_[0].flowFine[0].view);
                writeImage(s, 3, CIS, levels_v_[0].flowFine[1].view);
                writeImage(s, 4, SI, (sharpen_ ? sharp_[o] : out_[o]).view);
                writeBuffer(s, 5, stateBuf_.buffer);
                writeImage(s, 6, SI, splatMap_.view);
                writeImage(s, 7, CIS, prevBwd_.view);
                writeImage(s, 8, CIS, staticMask_);
                synthSets_.push_back(s);
                dsInterp_.push_back(s);
            }
            if (dupReal_) {
                VkDescriptorSet s = allocSet(interp_.setLayout);
                writeImage(s, 0, CIS, sources_[prev].view);
                writeImage(s, 1, CIS, sources_[cur].view);
                writeImage(s, 2, CIS, levels_v_[0].flowFine[0].view);
                writeImage(s, 3, CIS, levels_v_[0].flowFine[1].view);
                writeImage(s, 4, SI, (sharpen_ ? sharpReal_ : outReal_).view);
                writeBuffer(s, 5, stateBuf_.buffer);
                writeImage(s, 6, SI, splatMap_.view);
                writeImage(s, 7, CIS, prevBwd_.view);
                writeImage(s, 8, CIS, staticMask_);
                synthSets_.push_back(s);
                dsInterpReal_.push_back(s);
            }
        }
    }
}

// ------------------------------------------------ FgContext (optional passes)

void FrameGen::destroyPipeline(FgPipeline& p) {
    auto& vt = dev_.vt;
    if (p.pipeline) vt.DestroyPipeline(dev_.device, p.pipeline, nullptr);
    if (p.layout) vt.DestroyPipelineLayout(dev_.device, p.layout, nullptr);
    if (p.setLayout) vt.DestroyDescriptorSetLayout(dev_.device, p.setLayout, nullptr);
    p = FgPipeline{};
}

AllocatedImage FrameGen::createImage(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage) {
    AllocatedImage img = dev_.createImage(format, extent, usage, true);
    dev_.immediate([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier b = imageBarrier(img.image, 0, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                                              VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        dev_.vt.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                   0, nullptr, 1, &b);
    });
    return img;
}

FgFineLevel FrameGen::fineLevel() const {
    const Level& l0 = levels_v_[0];
    FgFineLevel f;
    f.luma = l0.size;
    f.fine = l0.fine;
    f.block = l0.refined ? kFineBlock : kBlockSize;
    for (int i = 0; i < 2; ++i) {
        f.pyr[i] = l0.pyr[i].view;
        f.flow[i] = l0.flowFine[i].view;
    }
    return f;
}

VkDescriptorSet FrameGen::synthesisSet(VkImageView prev, VkImageView cur, uint32_t output) {
    const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    VkDescriptorSet s = allocSet(interp_.setLayout);
    writeImage(s, 0, CIS, prev);
    writeImage(s, 1, CIS, cur);
    writeImage(s, 2, CIS, levels_v_[0].flowFine[0].view);
    writeImage(s, 3, CIS, levels_v_[0].flowFine[1].view);
    writeImage(s, 4, SI, (sharpen_ ? sharp_[output] : out_[output]).view);
    writeBuffer(s, 5, stateBuf_.buffer);
    writeImage(s, 6, SI, splatMap_.view);
    writeImage(s, 7, CIS, prevBwd_.view);
    writeImage(s, 8, CIS, staticMask_);
    synthSets_.push_back(s);
    return s;
}

void FrameGen::setStaticMask(VkImageView view) {
    staticMask_ = view;
    hasStaticMask_ = true;
    for (VkDescriptorSet s : synthSets_) writeImage(s, 8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, view);
}

VkDescriptorSet FrameGen::realOutputSet(VkImageView src) {
    VkDescriptorSet s = allocSet(upscale_.setLayout);
    writeImage(s, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, src);
    writeImage(s, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, (sharpen_ ? sharpReal_ : outReal_).view);
    return s;
}

void FrameGen::destroyAll() {
    auto& vt = dev_.vt;
    VkDevice d = dev_.device;
    for (auto& p : passes_) p->destroy(*this);  // (each copes with a partial create)
    for (VkDescriptorPool pool : pools_) vt.DestroyDescriptorPool(d, pool, nullptr);
    pools_.clear();
    planner_.reset();
    for (Pass* p : {&downsample_, &blockMatch_, &smooth_, &refine_, &reduce_, &interp_, &upscale_, &cas_, &splat_, &frameState_, &downChroma_}) {
        if (p->pipeline) vt.DestroyPipeline(d, p->pipeline, nullptr);
        if (p->layout) vt.DestroyPipelineLayout(d, p->layout, nullptr);
        if (p->setLayout) vt.DestroyDescriptorSetLayout(d, p->setLayout, nullptr);
        *p = Pass{};
    }
    if (sampler_) vt.DestroySampler(d, sampler_, nullptr);
    sampler_ = VK_NULL_HANDLE;
    for (auto& src : sources_) {
        if (src.view) vt.DestroyImageView(d, src.view, nullptr);
        src.view = VK_NULL_HANDLE;
    }
    for (auto& lv : levels_v_) {
        for (auto& i : lv.pyr) dev_.destroyImage(i);
        for (auto& i : lv.chroma) if (i.image) dev_.destroyImage(i);
        for (auto& i : lv.flowRaw) dev_.destroyImage(i);
        for (auto& i : lv.flow) dev_.destroyImage(i);
        if (lv.refined) for (auto& i : lv.flowFine) dev_.destroyImage(i);
        for (auto& i : lv.cost) dev_.destroyImage(i);
    }
    levels_v_.clear();
    for (auto& o : out_) dev_.destroyImage(o);
    out_.clear();
    if (splatMap_.image) dev_.destroyImage(splatMap_);
    if (prevBwd_.image) dev_.destroyImage(prevBwd_);
    if (chromaDummy_.image) dev_.destroyImage(chromaDummy_);
    if (maskDummy_.image) dev_.destroyImage(maskDummy_);
    staticMask_ = VK_NULL_HANDLE;
    hasStaticMask_ = false;
    synthSets_.clear();
    if (outReal_.image) dev_.destroyImage(outReal_);
    for (auto& s : sharp_) dev_.destroyImage(s);
    sharp_.clear();
    if (sharpReal_.image) dev_.destroyImage(sharpReal_);
    dev_.destroyBuffer(stateBuf_);
    if (stateMapped_) vt.UnmapMemory(d, stateReadback_.memory);
    stateMapped_ = nullptr;
    dev_.destroyBuffer(stateReadback_);
    if (queryPool_) vt.DestroyQueryPool(d, queryPool_, nullptr);
    queryPool_ = VK_NULL_HANDLE;
    for (size_t i = 0; i < dumpBuf_.size(); ++i) {
        if (dumpMapped_[i]) vt.UnmapMemory(d, dumpBuf_[i].memory);
        dumpMapped_[i] = nullptr;
        dev_.destroyBuffer(dumpBuf_[i]);
    }
    if (costMapped_) vt.UnmapMemory(d, costReadback_.memory);
    costMapped_ = nullptr;
    dev_.destroyBuffer(costReadback_);
}

void FrameGen::recordDump(VkCommandBuffer cmd, uint32_t which, uint32_t cur) {
    auto& vt = dev_.vt;
    // Everything dumped is at display size: every generated output and the upscaled real frame
    // (outReal_); the source is only dumped directly when not upscaling.
    const VkDeviceSize bytes = VkDeviceSize(displayExtent_.width) * displayExtent_.height * (enc_.is64 ? 8 : 4);
    if (!dumpBuf_[which].buffer) {
        dumpBuf_[which] = dev_.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        VK_CHECK(vt.MapMemory(dev_.device, dumpBuf_[which].memory, 0, VK_WHOLE_SIZE, 0, &dumpMapped_[which]));
    }
    if (outputs_ > 0 && !costReadback_.buffer) {
        costReadback_ = dev_.createBuffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        VK_CHECK(vt.MapMemory(dev_.device, costReadback_.memory, 0, VK_WHOLE_SIZE, 0, &costMapped_));
    }
    memoryBarrier(cmd);
    if (which < outputs_) {
        VkBufferCopy bc{0, 0, 16};
        vt.CmdCopyBuffer(cmd, stateBuf_.buffer, costReadback_.buffer, 1, &bc);
    }
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {displayExtent_.width, displayExtent_.height, 1};
    // Output, upscaled-real and history images all live in GENERAL layout.
    VkImage dumpSrc = which < outputs_ ? out_[which].image : (packReal_ ? outReal_.image : sources_[cur].image);
    vt.CmdCopyImageToBuffer(cmd, dumpSrc, VK_IMAGE_LAYOUT_GENERAL, dumpBuf_[which].buffer, 1, &region);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vt.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

// IEEE half -> float (for dumping RGBA16F swapchains).
static float halfToFloat(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1u, exp = (h >> 10) & 0x1fu, mant = h & 0x3ffu;
    float v;
    if (exp == 0) v = std::ldexp(float(mant), -24);
    else if (exp == 31) v = mant ? NAN : INFINITY;
    else v = std::ldexp(float(mant | 0x400u), int(exp) - 25);
    return sign ? -v : v;
}

// HDR dumps are written as SDR PPMs: the values are brought back to linear
// light with SDR white (203 nits) at 1, then sRGB-encoded, so an HDR run can
// be compared with the SDR ground truth it was made from.
static float srgbEncode(float v) {
    v = std::clamp(std::isfinite(v) ? v : 0.f, 0.f, 1.f);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
}

static float pqDecodeNits(float e) {  // SMPTE ST 2084 EOTF: code value -> nits
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    const float p = std::pow(std::clamp(e, 0.f, 1.f), 1.f / m2);
    return 10000.f * std::pow(std::max(p - c1, 0.f) / (c2 - c3 * p), 1.f / m1);
}

bool FrameGen::writeDump(const std::string& path, uint32_t which) {
    if (!dumpMapped_[which]) return false;
    if (which < outputs_ && costMapped_) BDEX_DBG("%s: mean matching cost %.4f", path.c_str(), *static_cast<const float*>(costMapped_));
    const uint32_t enc = enc_.encoding & 0xffu;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fprintf(f, "P6\n%u %u\n255\n", displayExtent_.width, displayExtent_.height);
    std::vector<uint8_t> row(displayExtent_.width * 3);
    const uint32_t* px = static_cast<const uint32_t*>(dumpMapped_[which]);
    for (uint32_t y = 0; y < displayExtent_.height; ++y) {
        for (uint32_t x = 0; x < displayExtent_.width; ++x) {
            uint8_t r, g, b;
            if (enc_.is64) {
                // RGBA16F: values as stored (linear, scRGB above 1 clipped).
                const uint16_t* h = reinterpret_cast<const uint16_t*>(px) + (size_t(y) * displayExtent_.width + x) * 4;
                const int32_t tr = enc_.transfer;
                auto q = [tr](uint16_t c) {
                    float f = halfToFloat(c);
                    if (tr == TRANSFER_SCRGB) f = srgbEncode(f * (80.f / 203.f));
                    else if (tr == TRANSFER_PQ) f = srgbEncode(pqDecodeNits(f) / 203.f);
                    return uint8_t(std::lround(std::clamp(std::isfinite(f) ? f : 0.f, 0.f, 1.f) * 255.f));
                };
                r = q(h[0]); g = q(h[1]); b = q(h[2]);
                row[x * 3] = r; row[x * 3 + 1] = g; row[x * 3 + 2] = b;
                continue;
            }
            uint32_t v = px[y * displayExtent_.width + x];
            if (enc == ENC_BGRA8) { b = v & 0xff; g = (v >> 8) & 0xff; r = (v >> 16) & 0xff; }
            else if (enc == ENC_RGBA8) { r = v & 0xff; g = (v >> 8) & 0xff; b = (v >> 16) & 0xff; }
            else if (enc_.transfer == TRANSFER_PQ) {
                auto q = [](uint32_t c) {
                    return uint8_t(std::lround(srgbEncode(pqDecodeNits(float(c & 1023u) / 1023.f) / 203.f) * 255.f));
                };
                if (enc == ENC_A2B10G10R10) { r = q(v); g = q(v >> 10); b = q(v >> 20); }
                else { b = q(v); g = q(v >> 10); r = q(v >> 20); }
            }
            else if (enc == ENC_A2B10G10R10) { r = (v & 1023) >> 2; g = ((v >> 10) & 1023) >> 2; b = ((v >> 20) & 1023) >> 2; }
            else { b = (v & 1023) >> 2; g = ((v >> 10) & 1023) >> 2; r = ((v >> 20) & 1023) >> 2; }
            row[x * 3] = r; row[x * 3 + 1] = g; row[x * 3 + 2] = b;
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
    return true;
}

// Full compute/transfer barrier: coarse but sufficient for a linear chain of
// passes on a single queue. It also covers indirect-argument reads: the
// gated dispatches read their arguments from the frame state buffer, which
// every pass binding it (whole-buffer descriptors) may have written.
void FrameGen::memoryBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                       VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    dev_.vt.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                                   VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                               0, 1, &mb, 0, nullptr, 0, nullptr);
}

void FrameGen::recordAcquireSources(VkCommandBuffer cmd, int prev, int cur) {
    for (auto& p : passes_) p->frameBegin(*this);  // one call per recorded frame, generating or not
    VkImageMemoryBarrier b[2];
    uint32_t n = 0;
    for (int idx : {prev, cur}) {
        if (idx < 0) continue;
        b[n++] = imageBarrier(sources_[idx].image, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
                              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                              VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_GENERAL);
    }
    dev_.vt.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, 0, nullptr, 0, nullptr, n, b);
}

void FrameGen::recordReleaseSources(VkCommandBuffer cmd, int prev, int cur) {
    VkImageMemoryBarrier b[2];
    uint32_t n = 0;
    for (int idx : {prev, cur}) {
        if (idx < 0) continue;
        b[n++] = imageBarrier(sources_[idx].image, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT,
                              VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    dev_.vt.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               0, 0, nullptr, 0, nullptr, n, b);
}

void FrameGen::recordAnalysis(VkCommandBuffer cmd, uint32_t parity, uint32_t cur) {
    auto& vt = dev_.vt;
    // A new frame: the per-frame state of the previous one no longer applies.
    flowLastFrame_ = flowThisFrame_;
    flowThisFrame_ = false;
    timingActive_ = false;
    gate_ = false;
    vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, downsample_.pipeline);
    for (int l = 0; l < levels_; ++l) {
        const Level& lv = levels_v_[l];
        DownsamplePC pc{{(int32_t)lv.size.width, (int32_t)lv.size.height}, l == 0 ? 1 : 0, enc_.transfer};
        vt.CmdPushConstants(cmd, downsample_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        const VkDescriptorSet* set = l == 0 ? &dsDownSrc_[parity][cur] : &dsDown_[parity][l];
        vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, downsample_.layout, 0, 1, set, 0, nullptr);
        vt.CmdDispatch(cmd, divUp(lv.size.width, 16), divUp(lv.size.height, 16), 1);
        memoryBarrier(cmd);
    }
    if (chroma_) {
        vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, downChroma_.pipeline);
        for (int l = 0; l < levels_; ++l) {
            const Level& lv = levels_v_[l];
            DownsamplePC pc{{(int32_t)lv.size.width, (int32_t)lv.size.height}, l == 0 ? 1 : 0, enc_.transfer};
            vt.CmdPushConstants(cmd, downChroma_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            const VkDescriptorSet* set = l == 0 ? &dsChromaSrc_[parity][cur] : &dsChroma_[parity][l];
            vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, downChroma_.layout, 0, 1, set, 0, nullptr);
            vt.CmdDispatch(cmd, divUp(lv.size.width, 16), divUp(lv.size.height, 16), 1);
            memoryBarrier(cmd);
        }
    }
    for (auto& p : passes_) p->analysisDone(*this, cmd, parity);
}

// Compute and indirect-argument reads of the state pass's results.
void FrameGen::stateBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                       VK_ACCESS_TRANSFER_READ_BIT;
    dev_.vt.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, 1, &mb, 0, nullptr, 0, nullptr);
}

// A dispatch the state pass may cancel (entry of gateFull_): indirect when
// gating is on this frame, otherwise the plain dispatch of the same size.
void FrameGen::dispatchGated(VkCommandBuffer cmd, uint32_t entry, uint32_t x, uint32_t y) {
    if (gate_) dev_.vt.CmdDispatchIndirect(cmd, stateBuf_.buffer, kStateGateActive + 12 * VkDeviceSize(entry));
    else dev_.vt.CmdDispatch(cmd, x, y, 1);
}

void FrameGen::recordFrameState(VkCommandBuffer cmd, const FrameTiming& timing) {
    timingActive_ = false;
    gate_ = false;
    if (outputs_ == 0 || (!(*cfgp_).frameTiming && !dedup_ && !timing.haveDisplay)) return;
    auto& vt = dev_.vt;
    const Level& l0 = levels_v_[0];
    const bool reset = timing.reset || !(timing.sinceLastMs > 0.0);
    // A repeat keeps the previous pair's flow: only possible if that flow
    // was computed (not after a frame presented without generation).
    const bool dedup = dedup_ && !reset && flowLastFrame_;
    vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, frameState_.pipeline);
    vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, frameState_.layout, 0, 1, &dsState_, 0, nullptr);
    FrameStatePC pc{{(int32_t)l0.size.width, (int32_t)l0.size.height}, 0, timing.multiplier,
                    float(timing.sinceLastMs), float(timing.intervalMs),
                    (reset ? kStateReset : 0) | (dedup ? kStateDedup : 0) | ((*cfgp_).extrapolate ? kStateExtrap : 0) |
                        (timing.haveDisplay ? kStateDisplay : 0) |
                        ((*cfgp_).frameTiming || dedup_ ? kStateContent : 0),
                    static_cast<int32_t>(gateFull_.size() / 4),
                    // A few differing pixels (a blinking cursor, dithering) still count as a repeat.
                    std::max(4u, l0.size.width * l0.size.height / 16384u),
                    {timing.displayMs[0], timing.displayMs[1], timing.displayMs[2], timing.displayMs[3]}};
    if (dedup) {
        vt.CmdFillBuffer(cmd, stateBuf_.buffer, kStateDiffCount, 4, 0);
        vt.CmdFillBuffer(cmd, stateBuf_.buffer, kStateTileDiff, 64 * 4, 0);
        memoryBarrier(cmd);
        vt.CmdPushConstants(cmd, frameState_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vt.CmdDispatch(cmd, divUp(l0.size.width, 16), divUp(l0.size.height, 16), 1);
        memoryBarrier(cmd);
    }
    pc.mode = 1;
    vt.CmdPushConstants(cmd, frameState_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vt.CmdDispatch(cmd, 1, 1, 1);
    stateBarrier(cmd);
    VkBufferCopy bc{0, 0, kStateHead};
    vt.CmdCopyBuffer(cmd, stateBuf_.buffer, stateReadback_.buffer, 1, &bc);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vt.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    timingActive_ = true;
    gate_ = dedup;
}

bool FrameGen::readState(State& out) const {
    if (!stateMapped_) return false;
    const auto* w = static_cast<const uint32_t*>(stateMapped_);
    out.dup = w[1];
    std::memcpy(&out.intervalMs, w + 5, 4);
    out.repeats = w[6];
    std::memcpy(out.t, w + 8, sizeof(out.t));
    return true;
}

void FrameGen::recordFlow(VkCommandBuffer cmd, uint32_t parity) {
    auto& vt = dev_.vt;
    // The flow images still hold the previous pair's flow (online_tune rates it).
    for (auto& p : passes_) p->flowBegin(*this, cmd, parity);
    const Config cfg = effectiveConfig();
    flowThisFrame_ = true;
    // extrapolate_accel: keep the previous pair's finest backward flow before
    // this frame overwrites it (on a repeat, dedup keeps the flow, so the
    // copy equals it and the acceleration is zero).
    accelThisFrame_ = 0.f;
    bool keepPrevious = cfg.extrapolate && cfg.extrapolateAccel > 0.f;
    for (auto& p : passes_) keepPrevious = keepPrevious || p->needsPreviousFlow(cfg);
    if (keepPrevious && flowLastFrame_) {
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {levels_v_[0].fine.width, levels_v_[0].fine.height, 1};
        vt.CmdCopyImage(cmd, levels_v_[0].flowFine[1].image, VK_IMAGE_LAYOUT_GENERAL, prevBwd_.image,
                        VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        memoryBarrier(cmd);
        if (cfg.extrapolate) accelThisFrame_ = cfg.extrapolateAccel;
    }
    for (int l = levels_ - 1; l >= 0; --l) {
        const Level& lv = levels_v_[l];
        vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blockMatch_.pipeline);
        MatchPC pc{{(int32_t)lv.size.width, (int32_t)lv.size.height},
                   {(int32_t)lv.blocks.width, (int32_t)lv.blocks.height},
                   l + 1 < levels_ ? cfg.searchFine : cfg.searchRadius, l + 1 < levels_ ? 1 : 0, cfg.smoothness, cfg.zeroBias,
                   cfg.gradWeight, cfg.censusWeight, cfg.censusMargin, cfg.rotWeight, 0, cfg.zeroBiasContrast,
                   chroma_ ? cfg.chromaWeight : 0.f, 0};
        const bool temporal = cfg.temporalPrediction && flowValid_;
        for (uint32_t dir = 0; dir < 2; ++dir) {
            pc.temporal = temporal ? static_cast<int32_t>(dir + 1) : 0;
            pc.chromaHistory = flowValid_ ? static_cast<int32_t>(dir + 1) : 0;
            vt.CmdPushConstants(cmd, blockMatch_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blockMatch_.layout, 0, 1, &dsMatch_[parity][dir][l], 0, nullptr);
            dispatchGated(cmd, 3 * l, lv.blocks.width, lv.blocks.height);
        }
        memoryBarrier(cmd);

        vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, smooth_.pipeline);
        SizePC spc{{(int32_t)lv.blocks.width, (int32_t)lv.blocks.height}};
        vt.CmdPushConstants(cmd, smooth_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(spc), &spc);
        for (uint32_t dir = 0; dir < 2; ++dir) {
            vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, smooth_.layout, 0, 1, &dsSmooth_[dir][l], 0, nullptr);
            dispatchGated(cmd, 3 * l + 1, divUp(lv.blocks.width, 16), divUp(lv.blocks.height, 16));
        }
        memoryBarrier(cmd);

        if (!lv.refined) continue;  // flowFine aliases the smoothed flow (see createResources)
        vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, refine_.pipeline);
        RefinePC rpc{{(int32_t)lv.size.width, (int32_t)lv.size.height},
                     {(int32_t)lv.blocks.width, (int32_t)lv.blocks.height},
                     {(int32_t)lv.fine.width, (int32_t)lv.fine.height},
                     (int32_t)kBlockSize, (int32_t)kFineBlock, cfg.refineBias, cfg.subpixel && l == 0 ? 1 : 0};
        vt.CmdPushConstants(cmd, refine_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rpc), &rpc);
        for (uint32_t dir = 0; dir < 2; ++dir) {
            vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, refine_.layout, 0, 1, &dsRefine_[parity][dir][l], 0, nullptr);
            dispatchGated(cmd, 3 * l + 2, divUp(lv.fine.width, 16), divUp(lv.fine.height, 16));
        }
        memoryBarrier(cmd);
    }

    const Level& fine = levels_v_[0];
    for (auto& p : passes_) p->fineFlow(*this, cmd, parity, cfg);
    vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, reduce_.pipeline);
    const uint32_t tiles = cfg.sceneCutLocal ? kCutTiles : 0;
    ReducePC rpc{{(int32_t)fine.blocks.width, (int32_t)fine.blocks.height}, (int32_t)tiles};
    vt.CmdPushConstants(cmd, reduce_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rpc), &rpc);
    vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, reduce_.layout, 0, 1, &dsReduce_, 0, nullptr);
    dispatchGated(cmd, gateReduce_, 1 + tiles * tiles, 1);
    memoryBarrier(cmd);
    flowValid_ = true;  // from now on the flow images hold a real previous flow
}

void FrameGen::copyToPresent(VkCommandBuffer cmd, VkImage src, VkImageLayout srcLayout, VkAccessFlags srcAccess,
                             VkPipelineStageFlags srcStage, VkImage dst) {
    auto& vt = dev_.vt;
    VkImageMemoryBarrier pre[2] = {
        imageBarrier(src, srcAccess, VK_ACCESS_TRANSFER_READ_BIT, srcLayout, srcLayout),
        imageBarrier(dst, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
    };
    // The swapchain image's acquire semaphore is waited at the transfer stage,
    // so the layout transition must be ordered after that stage as well.
    vt.CmdPipelineBarrier(cmd, srcStage | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                          nullptr, 2, pre);
    // The internal images (out_, outReal_) are display-sized.
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {displayExtent_.width, displayExtent_.height, 1};
    vt.CmdCopyImage(cmd, src, srcLayout, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    VkImageMemoryBarrier post = imageBarrier(dst, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
                                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vt.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &post);
}

void FrameGen::recordInterpolate(VkCommandBuffer cmd, uint32_t prev, uint32_t cur, float t, uint32_t output) {
    const uint32_t N = static_cast<uint32_t>(sources_.size());
    VkDescriptorSet set = dsInterp_[(prev * N + cur) * outputs_ + output];
    for (auto& p : passes_)
        if (VkDescriptorSet alt = p->synthesisInputs(*this, cmd, prev, cur, output)) set = alt;
    recordSynthesis(cmd, set, t, timingActive_ ? int32_t(output) + 1 : 0, false);
    if (sharpen_) recordCas(cmd, dsCas_[output]);  // sharp_[output] -> out_[output]
}

// One synthesis pass (plus the forward splat it may need) into the output
// image of `set`. `timing` > 0 takes t from slot timing-1 of the state
// buffer instead of `t` (which still tells extrapolation from interpolation).
// `gated`: dispatched through the state pass's "only on duplicates" entry.
void FrameGen::recordSynthesis(VkCommandBuffer cmd, VkDescriptorSet set, float t, int32_t timing, bool gated) {
    auto& vt = dev_.vt;
    const Config cfg = effectiveConfig();
    const Level& fine = levels_v_[0];
    const bool splat = splatting_ && t > 1.f;
    const uint32_t gx = divUp(displayExtent_.width, 16), gy = divUp(displayExtent_.height, 16);
    auto dispatch = [&] {
        if (gated) vt.CmdDispatchIndirect(cmd, stateBuf_.buffer, kStateGateActive + 12 * VkDeviceSize(gateDupReal_));
        else vt.CmdDispatch(cmd, gx, gy, 1);
    };
    if (splat) {
        // Forward-splat the current frame to time t (see splat.comp).
        const uint32_t blkS = fine.refined ? kFineBlock : kBlockSize;
        const float sxS = float(displayExtent_.width) / extent_.width;
        const float syS = float(displayExtent_.height) / extent_.height;
        VkClearColorValue none{};
        none.uint32[0] = 0xFFFFFFFFu;
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vt.CmdClearColorImage(cmd, splatMap_.image, VK_IMAGE_LAYOUT_GENERAL, &none, 1, &range);
        memoryBarrier(cmd);
        vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, splat_.pipeline);
        SplatPC spc{{(int32_t)displayExtent_.width, (int32_t)displayExtent_.height}, t - 1.f, flowScale_ * sxS,
                    {1.f / (fine.fine.width * blkS * flowScale_ * sxS), 1.f / (fine.fine.height * blkS * flowScale_ * syS)},
                    timing, accelThisFrame_};
        vt.CmdPushConstants(cmd, splat_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(spc), &spc);
        vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, splat_.layout, 0, 1, &dsSplat_, 0, nullptr);
        dispatch();
        memoryBarrier(cmd);
    }
    vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, interp_.pipeline);
    // Interpolation output is at display resolution; the flow (in render pixels)
    // and its coverage are scaled to display space, so the same shader also
    // upscales the generated frame when render != display.
    const uint32_t blk = fine.refined ? kFineBlock : kBlockSize;
    const float sx = float(displayExtent_.width) / extent_.width;
    const float sy = float(displayExtent_.height) / extent_.height;
    InterpPC pc{{(int32_t)displayExtent_.width, (int32_t)displayExtent_.height}, t, flowScale_ * sx, enc_.encoding,
                cfg.debug == Config::Debug::Flow ? 1 : cfg.debug == Config::Debug::Split ? 2 : 0,
                cfg.sceneCutLow, cfg.sceneCutHigh,
                {1.f / (fine.fine.width * blk * flowScale_ * sx),
                 1.f / (fine.fine.height * blk * flowScale_ * sy)},
                cfg.flowIterations, hudMode_, hudGame_, hudOut_, hudLow1_, hudLow01_, cfg.occlusionStrength, cfg.staticGuard, cfg.extrapolateHold, splat ? 1 : 0, cfg.pixelVectors ? 1 : 0, enc_.transfer,
                timing, cfg.sceneCutLocal ? (int32_t)kCutTiles : 0, t > 1.f ? accelThisFrame_ : 0.f, hudLatency_,
                cfg.synthBicubic ? 1 : 0, hasStaticMask_ ? float(cfg.hudMask) : 0.f};
    vt.CmdPushConstants(cmd, interp_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, interp_.layout, 0, 1, &set, 0, nullptr);
    dispatch();
    // Make the previous output copy (worker submission) precede this overwrite.
    memoryBarrier(cmd);
}

void FrameGen::recordCopyOutput(VkCommandBuffer cmd, uint32_t output, VkImage dst) {
    copyToPresent(cmd, out_[output].image, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, dst);
}

void FrameGen::recordCopySource(VkCommandBuffer cmd, uint32_t src, VkImage dst) {
    auto& vt = dev_.vt;
    if (packReal_) {  // the real frame was produced into outReal_ by recordUpscaleReal
        copyToPresent(cmd, outReal_.image, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, dst);
        return;
    }
    VkImage image = sources_[src].image;
    VkImageMemoryBarrier pre[2] = {
        imageBarrier(image, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
        imageBarrier(dst, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
    };
    vt.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, pre);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {extent_.width, extent_.height, 1};
    vt.CmdCopyImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    VkImageMemoryBarrier post[2] = {
        imageBarrier(image, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
        imageBarrier(dst, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
    };
    vt.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 2, post);
}

} // namespace bdex

namespace bdex {

void FrameGen::profileBegin(VkCommandBuffer cmd, uint32_t slot) {
    // The slot's previous submission has completed (online_tune reads back).
    for (auto& p : passes_) p->slotBegin(*this, slot);
    if (!queryPool_ || slot >= kProfileSlots) return;
    const uint32_t base = slot * (StageCount + 1);
    dev_.vt.CmdResetQueryPool(cmd, queryPool_, base, StageCount + 1);
    // Bottom-of-pipe so that a semaphore wait attached to the submission
    // (swapchain acquire) is not counted as GPU time.
    dev_.vt.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool_, base);
    for (auto& w : profileWritten_[slot]) w = false;
    profileWritten_[slot][0] = true;
}

void FrameGen::profileMark(VkCommandBuffer cmd, uint32_t slot, Stage stage) {
    if (!queryPool_ || slot >= kProfileSlots) return;
    dev_.vt.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool_, slot * (StageCount + 1) + stage + 1);
    profileWritten_[slot][stage + 1] = true;
}

void FrameGen::profileCollect(uint32_t slot) {
    if (!queryPool_ || slot >= kProfileSlots || !profileWritten_[slot][0]) return;
    uint64_t ts[StageCount + 1]{};
    const uint32_t base = slot * (StageCount + 1);
    // Query each written timestamp individually (unwritten ones would block).
    for (uint32_t i = 0; i <= StageCount; ++i) {
        if (!profileWritten_[slot][i]) continue;
        dev_.vt.GetQueryPoolResults(dev_.device, queryPool_, base + i, 1, sizeof(uint64_t), &ts[i], sizeof(uint64_t),
                                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    }
    uint64_t last = ts[0];
    for (uint32_t s = 0; s < StageCount; ++s) {
        if (!profileWritten_[slot][s + 1]) continue;
        const double ms = double(ts[s + 1] - last) * timestampPeriodNs_ * 1e-6;
        profileMs_[s] += ms;
        profileFrames_[s] += 1;
        if (s != StageCopy) budgetMs_ += ms;
        if (s == StageAnalysis) ++budgetFrames_;
        last = ts[s + 1];
    }
    profileWritten_[slot][0] = false;
}

double FrameGen::takeGpuMs() {
    const double ms = budgetFrames_ ? budgetMs_ / budgetFrames_ : 0.0;
    budgetMs_ = 0.0;
    budgetFrames_ = 0;
    return ms;
}

Config FrameGen::effectiveConfig() const {
    Config c = (*cfgp_);
    for (const auto& p : passes_) p->overrideConfig(c);
    if (qualityLevel_ >= 1) {
        c.pixelVectors = false;
        c.subpixel = false;
        c.rotWeight = c.censusWeight = c.gradWeight = c.chromaWeight = 0.f;
        c.extrapolateAccel = 0.f;
        c.occlusionStrength = 0.f;
        c.flowIterations = std::min(c.flowIterations, 1);
    }
    if (qualityLevel_ >= 2) {
        c.searchFine = 1;
        c.flowIterations = 0;
    }
    return c;
}

std::string FrameGen::profileReport() {
    if (!queryPool_ || !(*cfgp_).profile) return "";
    static const char* names[StageCount] = {"analysis", "flow", "interp", "copy"};
    char buf[256];
    std::string out = " | gpu ms:";
    for (uint32_t s = 0; s < StageCount; ++s) {
        snprintf(buf, sizeof(buf), " %s %.2f", names[s], profileFrames_[s] ? profileMs_[s] / profileFrames_[s] : 0.0);
        out += buf;
        profileMs_[s] = 0;
        profileFrames_[s] = 0;
    }
    return out;
}

} // namespace bdex
