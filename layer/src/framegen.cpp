#include "framegen.h"
#include "log.h"
#include "shaders.h"
#include "vk_util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace bdex {

namespace {

struct DownsamplePC { int32_t size[2]; int32_t fromColor; int32_t srgbSource; };
struct MatchPC { int32_t size[2]; int32_t blocks[2]; int32_t radius; int32_t hasCoarse; float smoothness; float zeroBias; };
struct SizePC { int32_t size[2]; };
struct RefinePC { int32_t size[2]; int32_t blocks[2]; int32_t fine[2]; int32_t coarseBlock; int32_t fineBlock; float ownBias; };
struct InterpPC { int32_t size[2]; float t; float flowScale; uint32_t encoding; int32_t debugMode; float cutLow; float cutHigh; float flowInvSize[2]; int32_t iterations; };
struct UpscalePC { int32_t outSize[2]; int32_t srcSize[2]; uint32_t encoding; int32_t filter; };
struct CasPC { int32_t size[2]; uint32_t encoding; float sharpness; };

constexpr uint32_t kBlockSize = 8;
constexpr uint32_t kFineBlock = 4;
constexpr uint32_t kPushSize = 64;

uint32_t divUp(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

} // namespace

FrameGen::FrameGen(DeviceData& dev, VkFormat format, VkExtent2D renderExtent, VkExtent2D displayExtent, uint32_t outputs,
                   const std::vector<VkImage>& sources)
    : dev_(dev), format_(format), extent_(renderExtent), displayExtent_(displayExtent), outputs_(outputs) {
    upscaling_ = displayExtent_.width != extent_.width || displayExtent_.height != extent_.height;
    // The CAS pass needs a linear rgba16f intermediate it can both write and
    // sample with filtering; skip sharpening if the driver cannot provide it.
    sharpen_ = upscaling_ && dev.config.sharpness > 0.f &&
               dev.formatSupports(VK_FORMAT_R16G16B16A16_SFLOAT,
                                  VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    enc_ = encodingForFormat(format);
    if (!enc_.supported) throw VkError(VK_ERROR_FORMAT_NOT_SUPPORTED, "swapchain format not supported by frame generation");
    for (VkImage img : sources) sources_.push_back({img, VK_NULL_HANDLE});
    if (outputs_ == 0 && !upscaling_) return;  // pass-through: only recordCopySource is used
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
                  filt[std::clamp(dev_.config.upscaleFilter, 0, 2)],
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
            blockMatch_ = makePass(block_match_comp_f16, block_match_comp_f16_size, {SI, SI, CIS, SI, SI});
            refine_ = makePass(flow_refine_comp_f16, flow_refine_comp_f16_size, {SI, SI, SI, SI, SI});
        } else {
            downsample_ = makePass(downsample_comp_f32, downsample_comp_f32_size, {CIS, SI});
            blockMatch_ = makePass(block_match_comp_f32, block_match_comp_f32_size, {SI, SI, CIS, SI, SI});
            refine_ = makePass(flow_refine_comp_f32, flow_refine_comp_f32_size, {SI, SI, SI, SI, SI});
        }
        smooth_ = makePass(flow_smooth_comp, flow_smooth_comp_size, {SI, SI});
        reduce_ = makePass(reduce_cost_comp, reduce_cost_comp_size, {SI, SB});
        // With sharpening, the interpolation writes a linear rgba16f image that
        // the CAS pass then sharpens and packs; otherwise it packs directly.
        if (sharpen_)       interp_ = makePass(interpolate_comp_sharp, interpolate_comp_sharp_size, {CIS, CIS, CIS, CIS, SI, SB});
        else if (enc_.is64) interp_ = makePass(interpolate_comp_u64, interpolate_comp_u64_size, {CIS, CIS, CIS, CIS, SI, SB});
        else                interp_ = makePass(interpolate_comp_u32, interpolate_comp_u32_size, {CIS, CIS, CIS, CIS, SI, SB});
    }
    if (upscaling_) {
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
    const Config& cfg = dev_.config;
    const VkImageUsageFlags storageSampled = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    if (!dev_.formatSupports(format_, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
        throw VkError(VK_ERROR_FORMAT_NOT_SUPPORTED, "swapchain format cannot be sampled");
    if (!dev_.formatSupports(enc_.storageFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
        throw VkError(VK_ERROR_FORMAT_NOT_SUPPORTED, "output storage format unsupported");

    // Pyramid: level 0 at full or half resolution, then halving. Stop early
    // when a level would have fewer than 2x2 blocks.
    const int fs = cfg.flowScaleFor(extent_.width, extent_.height);
    VkExtent2D size = {divUp(extent_.width, fs), divUp(extent_.height, fs)};
    flowScale_ = float(fs);
    for (int l = 0; outputs_ > 0 && l < cfg.levels; ++l) {
        Level lv;
        lv.size = size;
        lv.blocks = {divUp(size.width, kBlockSize), divUp(size.height, kBlockSize)};
        lv.fine = {divUp(size.width, kFineBlock), divUp(size.height, kFineBlock)};
        if (l > 0 && (lv.blocks.width < 2 || lv.blocks.height < 2)) break;
        for (int p = 0; p < 2; ++p) lv.pyr[p] = dev_.createImage(lumaFormat_, lv.size, storageSampled, true);
        for (int d = 0; d < 2; ++d) {
            lv.flowRaw[d] = dev_.createImage(VK_FORMAT_R32G32_SFLOAT, lv.blocks, storageSampled, true);
            lv.flow[d] = dev_.createImage(VK_FORMAT_R32G32_SFLOAT, lv.blocks, storageSampled, true);
            // Refinement runs on the finest level only unless refine_all is
            // set: coarser levels just predict the search centre, where the
            // smoothed flow is good enough.
            const bool refined = cfg.refine && (l == 0 || cfg.refineAll);
            lv.refined = refined;
            if (refined) lv.flowFine[d] = dev_.createImage(VK_FORMAT_R32G32_SFLOAT, lv.fine, storageSampled, true);
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
    if (upscaling_) outReal_ = dev_.createImage(enc_.storageFormat, displayExtent_, outUsage, true);
    if (sharpen_) {
        const VkImageUsageFlags sharpUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        for (uint32_t i = 0; i < outputs_; ++i)
            sharp_.push_back(dev_.createImage(VK_FORMAT_R16G16B16A16_SFLOAT, displayExtent_, sharpUsage, true));
        sharpReal_ = dev_.createImage(VK_FORMAT_R16G16B16A16_SFLOAT, displayExtent_, sharpUsage, true);
    }
    if (outputs_ > 0)
        costBuf_ = dev_.createBuffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    if (cfg.profile) {
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
            for (auto& i : lv.flowRaw) add(i);
            for (auto& i : lv.flow) add(i);
            if (lv.refined) for (auto& i : lv.flowFine) add(i);
            for (auto& i : lv.cost) add(i);
        }
        for (auto& o : out_) add(o);
        if (upscaling_) add(outReal_);
        for (auto& s : sharp_) add(s);
        if (sharpen_) add(sharpReal_);
        dev_.vt.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                   nullptr, static_cast<uint32_t>(barriers.size()), barriers.data());
        if (costBuf_.buffer) dev_.vt.CmdFillBuffer(cmd, costBuf_.buffer, 0, VK_WHOLE_SIZE, 0);
    });
}

VkDescriptorSet FrameGen::allocSet(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
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
    const uint32_t nInterp = N * N * O;
    const uint32_t gen = O > 0 ? 1 : 0;           // generation descriptor sets exist only with outputs
    const uint32_t up = upscaling_ ? N : 0;       // one upscale set per source
    const uint32_t cas = sharpen_ ? O + 1 : 0;    // one CAS set per generated output, plus the real frame
    const uint32_t nSets = gen * (2 * N + 2 * L + 4 * L + 2 * L + 4 * L + 1 + nInterp) + up + cas;
    const uint32_t nCIS = gen * (2 * N + 2 * L + 4 * L + 4 * nInterp) + up + cas;
    const uint32_t nSI = gen * (2 * N + 2 * L + 4 * L * 4 + 2 * L * 2 + 4 * L * 5 + 1 + nInterp) + up + cas;
    const uint32_t nSB = gen * (1 + nInterp);
    std::vector<VkDescriptorPoolSize> sizes;
    if (nCIS) sizes.push_back({VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nCIS});
    if (nSI) sizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, nSI});
    if (nSB) sizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nSB});
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = nSets;
    pi.poolSizeCount = static_cast<uint32_t>(sizes.size());
    pi.pPoolSizes = sizes.data();
    VK_CHECK(dev_.vt.CreateDescriptorPool(dev_.device, &pi, nullptr, &pool_));

    const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    const VkDescriptorType SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

    if (upscaling_) {
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
    writeBuffer(dsReduce_, 1, costBuf_.buffer);

    for (uint32_t prev = 0; prev < N; ++prev) {
        for (uint32_t cur = 0; cur < N; ++cur) {
            for (uint32_t o = 0; o < O; ++o) {
                VkDescriptorSet s = allocSet(interp_.setLayout);
                writeImage(s, 0, CIS, sources_[prev].view);
                writeImage(s, 1, CIS, sources_[cur].view);
                writeImage(s, 2, CIS, levels_v_[0].flowFine[0].view);
                writeImage(s, 3, CIS, levels_v_[0].flowFine[1].view);
                writeImage(s, 4, SI, (sharpen_ ? sharp_[o] : out_[o]).view);
                writeBuffer(s, 5, costBuf_.buffer);
                dsInterp_.push_back(s);
            }
        }
    }
}

void FrameGen::destroyAll() {
    auto& vt = dev_.vt;
    VkDevice d = dev_.device;
    if (pool_) vt.DestroyDescriptorPool(d, pool_, nullptr);
    pool_ = VK_NULL_HANDLE;
    for (Pass* p : {&downsample_, &blockMatch_, &smooth_, &refine_, &reduce_, &interp_, &upscale_, &cas_}) {
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
        for (auto& i : lv.flowRaw) dev_.destroyImage(i);
        for (auto& i : lv.flow) dev_.destroyImage(i);
        if (lv.refined) for (auto& i : lv.flowFine) dev_.destroyImage(i);
        for (auto& i : lv.cost) dev_.destroyImage(i);
    }
    levels_v_.clear();
    for (auto& o : out_) dev_.destroyImage(o);
    out_.clear();
    if (outReal_.image) dev_.destroyImage(outReal_);
    for (auto& s : sharp_) dev_.destroyImage(s);
    sharp_.clear();
    if (sharpReal_.image) dev_.destroyImage(sharpReal_);
    dev_.destroyBuffer(costBuf_);
    if (queryPool_) vt.DestroyQueryPool(d, queryPool_, nullptr);
    queryPool_ = VK_NULL_HANDLE;
    for (int i = 0; i < 2; ++i) {
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
    // Everything dumped is at display size: out_[0] and the upscaled real frame
    // (outReal_); the source is only dumped directly when not upscaling.
    const VkDeviceSize bytes = VkDeviceSize(displayExtent_.width) * displayExtent_.height * (enc_.is64 ? 8 : 4);
    if (!dumpBuf_[which].buffer) {
        dumpBuf_[which] = dev_.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        VK_CHECK(vt.MapMemory(dev_.device, dumpBuf_[which].memory, 0, VK_WHOLE_SIZE, 0, &dumpMapped_[which]));
    }
    if (!costReadback_.buffer) {
        costReadback_ = dev_.createBuffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        VK_CHECK(vt.MapMemory(dev_.device, costReadback_.memory, 0, VK_WHOLE_SIZE, 0, &costMapped_));
    }
    memoryBarrier(cmd);
    if (which == 0) {
        VkBufferCopy bc{0, 0, 16};
        vt.CmdCopyBuffer(cmd, costBuf_.buffer, costReadback_.buffer, 1, &bc);
    }
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {displayExtent_.width, displayExtent_.height, 1};
    // Output, upscaled-real and history images all live in GENERAL layout.
    VkImage dumpSrc = which == 0 ? out_[0].image : (upscaling_ ? outReal_.image : sources_[cur].image);
    vt.CmdCopyImageToBuffer(cmd, dumpSrc, VK_IMAGE_LAYOUT_GENERAL, dumpBuf_[which].buffer, 1, &region);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vt.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

bool FrameGen::writeDump(const std::string& path, uint32_t which) {
    if (!dumpMapped_[which] || enc_.is64) return false;
    if (which == 0 && costMapped_) BDEX_DBG("%s: mean matching cost %.4f", path.c_str(), *static_cast<const float*>(costMapped_));
    const uint32_t enc = enc_.encoding & 0xffu;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fprintf(f, "P6\n%u %u\n255\n", displayExtent_.width, displayExtent_.height);
    std::vector<uint8_t> row(displayExtent_.width * 3);
    const uint32_t* px = static_cast<const uint32_t*>(dumpMapped_[which]);
    for (uint32_t y = 0; y < displayExtent_.height; ++y) {
        for (uint32_t x = 0; x < displayExtent_.width; ++x) {
            uint32_t v = px[y * displayExtent_.width + x];
            uint8_t r, g, b;
            if (enc == ENC_BGRA8) { b = v & 0xff; g = (v >> 8) & 0xff; r = (v >> 16) & 0xff; }
            else if (enc == ENC_RGBA8) { r = v & 0xff; g = (v >> 8) & 0xff; b = (v >> 16) & 0xff; }
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
// passes on a single queue.
void FrameGen::memoryBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    dev_.vt.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr,
                               0, nullptr);
}

void FrameGen::recordAcquireSources(VkCommandBuffer cmd, int prev, int cur) {
    VkImageMemoryBarrier b[2];
    uint32_t n = 0;
    for (int idx : {prev, cur}) {
        if (idx < 0) continue;
        b[n++] = imageBarrier(sources_[idx].image, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
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
    vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, downsample_.pipeline);
    for (int l = 0; l < levels_; ++l) {
        const Level& lv = levels_v_[l];
        DownsamplePC pc{{(int32_t)lv.size.width, (int32_t)lv.size.height}, l == 0 ? 1 : 0, enc_.srgb ? 1 : 0};
        vt.CmdPushConstants(cmd, downsample_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        const VkDescriptorSet* set = l == 0 ? &dsDownSrc_[parity][cur] : &dsDown_[parity][l];
        vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, downsample_.layout, 0, 1, set, 0, nullptr);
        vt.CmdDispatch(cmd, divUp(lv.size.width, 16), divUp(lv.size.height, 16), 1);
        memoryBarrier(cmd);
    }
}

void FrameGen::recordFlow(VkCommandBuffer cmd, uint32_t parity) {
    auto& vt = dev_.vt;
    const Config& cfg = dev_.config;
    for (int l = levels_ - 1; l >= 0; --l) {
        const Level& lv = levels_v_[l];
        vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blockMatch_.pipeline);
        MatchPC pc{{(int32_t)lv.size.width, (int32_t)lv.size.height},
                   {(int32_t)lv.blocks.width, (int32_t)lv.blocks.height},
                   l + 1 < levels_ ? cfg.searchFine : cfg.searchRadius, l + 1 < levels_ ? 1 : 0, cfg.smoothness, cfg.zeroBias};
        vt.CmdPushConstants(cmd, blockMatch_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        for (uint32_t dir = 0; dir < 2; ++dir) {
            vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blockMatch_.layout, 0, 1, &dsMatch_[parity][dir][l], 0, nullptr);
            vt.CmdDispatch(cmd, lv.blocks.width, lv.blocks.height, 1);
        }
        memoryBarrier(cmd);

        vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, smooth_.pipeline);
        SizePC spc{{(int32_t)lv.blocks.width, (int32_t)lv.blocks.height}};
        vt.CmdPushConstants(cmd, smooth_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(spc), &spc);
        for (uint32_t dir = 0; dir < 2; ++dir) {
            vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, smooth_.layout, 0, 1, &dsSmooth_[dir][l], 0, nullptr);
            vt.CmdDispatch(cmd, divUp(lv.blocks.width, 16), divUp(lv.blocks.height, 16), 1);
        }
        memoryBarrier(cmd);

        if (!lv.refined) continue;  // flowFine aliases the smoothed flow (see createResources)
        vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, refine_.pipeline);
        RefinePC rpc{{(int32_t)lv.size.width, (int32_t)lv.size.height},
                     {(int32_t)lv.blocks.width, (int32_t)lv.blocks.height},
                     {(int32_t)lv.fine.width, (int32_t)lv.fine.height},
                     (int32_t)kBlockSize, (int32_t)kFineBlock, cfg.refineBias};
        vt.CmdPushConstants(cmd, refine_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rpc), &rpc);
        for (uint32_t dir = 0; dir < 2; ++dir) {
            vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, refine_.layout, 0, 1, &dsRefine_[parity][dir][l], 0, nullptr);
            vt.CmdDispatch(cmd, divUp(lv.fine.width, 16), divUp(lv.fine.height, 16), 1);
        }
        memoryBarrier(cmd);
    }

    const Level& fine = levels_v_[0];
    vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, reduce_.pipeline);
    SizePC rpc{{(int32_t)fine.blocks.width, (int32_t)fine.blocks.height}};
    vt.CmdPushConstants(cmd, reduce_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rpc), &rpc);
    vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, reduce_.layout, 0, 1, &dsReduce_, 0, nullptr);
    vt.CmdDispatch(cmd, 1, 1, 1);
    memoryBarrier(cmd);
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
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {extent_.width, extent_.height, 1};
    vt.CmdCopyImage(cmd, src, srcLayout, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    VkImageMemoryBarrier post = imageBarrier(dst, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
                                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vt.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &post);
}

void FrameGen::recordInterpolate(VkCommandBuffer cmd, uint32_t prev, uint32_t cur, float t, uint32_t output) {
    auto& vt = dev_.vt;
    const Config& cfg = dev_.config;
    vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, interp_.pipeline);
    const Level& fine = levels_v_[0];
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
                cfg.flowIterations};
    vt.CmdPushConstants(cmd, interp_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    const uint32_t N = static_cast<uint32_t>(sources_.size());
    vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, interp_.layout, 0, 1,
                             &dsInterp_[(prev * N + cur) * outputs_ + output], 0, nullptr);
    vt.CmdDispatch(cmd, divUp(displayExtent_.width, 16), divUp(displayExtent_.height, 16), 1);
    // Make the previous output copy (worker submission) precede this overwrite.
    memoryBarrier(cmd);
    if (sharpen_) recordCas(cmd, dsCas_[output]);  // sharp_[output] -> out_[output]
}

void FrameGen::recordCopyOutput(VkCommandBuffer cmd, uint32_t output, VkImage dst) {
    copyToPresent(cmd, out_[output].image, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, dst);
}

void FrameGen::recordUpscaleReal(VkCommandBuffer cmd, uint32_t cur) {
    auto& vt = dev_.vt;
    vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, upscale_.pipeline);
    UpscalePC pc{{(int32_t)displayExtent_.width, (int32_t)displayExtent_.height},
                 {(int32_t)extent_.width, (int32_t)extent_.height},
                 enc_.encoding, std::clamp(dev_.config.upscaleFilter, 0, 2)};
    vt.CmdPushConstants(cmd, upscale_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, upscale_.layout, 0, 1, &dsUpscale_[cur], 0, nullptr);
    vt.CmdDispatch(cmd, divUp(displayExtent_.width, 16), divUp(displayExtent_.height, 16), 1);
    // Order the CAS pass / the worker's copy after this write.
    memoryBarrier(cmd);
    if (sharpen_) recordCas(cmd, dsCasReal_);  // sharpReal_ -> outReal_
}

void FrameGen::recordCas(VkCommandBuffer cmd, VkDescriptorSet set) {
    auto& vt = dev_.vt;
    vt.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cas_.pipeline);
    CasPC pc{{(int32_t)displayExtent_.width, (int32_t)displayExtent_.height}, enc_.encoding, dev_.config.sharpness};
    vt.CmdPushConstants(cmd, cas_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vt.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cas_.layout, 0, 1, &set, 0, nullptr);
    vt.CmdDispatch(cmd, divUp(displayExtent_.width, 16), divUp(displayExtent_.height, 16), 1);
    memoryBarrier(cmd);
}

void FrameGen::recordCopySource(VkCommandBuffer cmd, uint32_t src, VkImage dst) {
    auto& vt = dev_.vt;
    if (upscaling_) {  // the real frame was upscaled into outReal_ by recordUpscaleReal
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
        profileMs_[s] += double(ts[s + 1] - last) * timestampPeriodNs_ * 1e-6;
        profileFrames_[s] += 1;
        last = ts[s + 1];
    }
    profileWritten_[slot][0] = false;
}

std::string FrameGen::profileReport() {
    if (!queryPool_) return "";
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
