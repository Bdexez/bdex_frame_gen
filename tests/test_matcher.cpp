// Host reference test of the block matcher (AGENTS.md, invariant n°1).
//
// Dispatches the real block_match.comp (f32 luma) on small images built so
// that many candidates tie exactly (luma in multiples of 1/64, zero penalties,
// flat and periodic content), reads the flow back, and compares every block's
// winner with a C++ model of the historical selection: each of the 64 threads
// keeps the first strict minimum over candidates tid, tid+64, ..., then a tree
// reduction (stride 32..1) keeps the lower thread on ties. The test also
// checks that the inputs actually discriminate that rule from a sequential
// "first index wins" scan — the substitution that caused the 0549e44
// regression — so it would have caught it. Needs a Vulkan device (lavapipe
// is fine), no display.
#include "device.h"
#include "shaders.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace bdex;

namespace {

constexpr int BS = 8, NT = 64;

struct MatchPC {
    int32_t size[2]; int32_t blocks[2]; int32_t radius; int32_t hasCoarse; float smoothness; float zeroBias;
    float gradWeight; float censusWeight; float censusMargin; float rotWeight; int32_t temporal; float zeroBiasContrast;
};

struct Image {
    int w, h;
    std::vector<float> v;
    float at(int x, int y) const {  // clamp to edge, like the shader's loads
        x = std::min(std::max(x, 0), w - 1);
        y = std::min(std::max(y, 0), h - 1);
        return v[size_t(y) * w + x];
    }
};

// Per-candidate cost exactly as the shader sums it (quadrants of 4x4, then /64).
float candCost(const Image& ref, const Image& tgt, int bx, int by, int dx, int dy) {
    float q[4];
    for (int quad = 0; quad < 4; ++quad) {
        int ox = (quad & 1) * 4, oy = (quad >> 1) * 4;
        float s = 0.f;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                int px = bx * BS + ox + x, py = by * BS + oy + y;
                s += std::fabs(ref.at(px, py) - tgt.at(px + dx, py + dy));
            }
        q[quad] = s;
    }
    return (q[0] + q[1] + q[2] + q[3]) * (1.f / float(NT));
}

// Winner index under the historical rule (strided per-thread minimum + tree).
int treeWinner(const std::vector<float>& c) {
    float best[NT];
    int idx[NT];
    for (int t = 0; t < NT; ++t) {
        best[t] = 1e30f;
        idx[t] = 0;
        for (int k = t; k < int(c.size()); k += NT)
            if (c[k] < best[t]) { best[t] = c[k]; idx[t] = k; }
    }
    for (int s = NT / 2; s > 0; s >>= 1)
        for (int t = 0; t < s; ++t)
            if (best[t + s] < best[t]) { best[t] = best[t + s]; idx[t] = idx[t + s]; }
    return idx[0];
}

int sequentialWinner(const std::vector<float>& c) {
    int bi = 0;
    for (int k = 1; k < int(c.size()); ++k)
        if (c[k] < c[bi]) bi = k;
    return bi;
}

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) { fprintf(stderr, "test_matcher: %s failed (%d)\n", what, int(r)); exit(1); }
}

}  // namespace

int main() {
    // ---- Inputs: 128x64, luma in multiples of 1/64 so every SAD is exact.
    const int W = 128, H = 64, R = 4;
    Image ref{W, H, std::vector<float>(size_t(W) * H)}, tgt = ref;
    std::mt19937 rng(1234);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float v;
            if (x < 32) v = 0.5f;                                          // flat: every candidate ties
            else if (x < 64) v = ((x + y) & 1) ? 0.25f : 0.75f;            // period 2: half the candidates tie
            else if (x < 96) v = float((x / 3 + y / 2) % 4) / 4.f;         // period 3 x 2 stripes
            else v = float(rng() % 64) / 64.f;                             // random: unique minima
            ref.v[size_t(y) * W + x] = v;
        }
    // Target: the reference shifted by (2, 1), so the textured parts have a
    // true motion while the periodic parts keep their ties.
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) tgt.v[size_t(y) * W + x] = ref.at(x - 2, y - 1);

    const int bw = W / BS, bh = H / BS;
    std::vector<int> expect(bw * bh);
    int discriminating = 0;
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            std::vector<float> c;
            for (int dy = -R; dy <= R; ++dy)
                for (int dx = -R; dx <= R; ++dx) c.push_back(candCost(ref, tgt, bx, by, dx, dy));
            expect[by * bw + bx] = treeWinner(c);
            discriminating += treeWinner(c) != sequentialWinner(c);
        }
    if (discriminating == 0) {
        fprintf(stderr, "test_matcher: inputs do not distinguish the tree reduction from a sequential scan\n");
        return 1;
    }

    // ---- Vulkan: a headless device and the layer's resource helpers.
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) { puts("no Vulkan, skipping"); return 77; }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    if (!n) { puts("no Vulkan device, skipping"); return 77; }
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(instance, &n, pds.data());
    VkPhysicalDevice pd = pds[0];
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf.data());
    uint32_t fam = 0;
    while (fam < nq && !(qf[fam].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++fam;
    float prio = 1.f;
    VkDeviceQueueCreateInfo dq{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    dq.queueFamilyIndex = fam;
    dq.queueCount = 1;
    dq.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &dq;
    VkDevice device;
    check(vkCreateDevice(pd, &dci, nullptr, &device), "vkCreateDevice");

    InstanceData inst;
    inst.vt.load(instance, vkGetInstanceProcAddr);
    inst.instance = instance;
    DeviceData dev;
    dev.vt.load(device, vkGetDeviceProcAddr);
    dev.inst = &inst;
    dev.physDev = pd;
    dev.device = device;
    vkGetPhysicalDeviceProperties(pd, &dev.props);
    vkGetPhysicalDeviceMemoryProperties(pd, &dev.memProps);
    vkGetDeviceQueue(device, fam, 0, &dev.queue);
    dev.queueFamily = fam;
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = fam;
    check(vkCreateCommandPool(device, &cp, nullptr, &dev.cmdPool), "vkCreateCommandPool");

    const VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    AllocatedImage imRef = dev.createImage(VK_FORMAT_R32_SFLOAT, {uint32_t(W), uint32_t(H)}, u, true);
    AllocatedImage imTgt = dev.createImage(VK_FORMAT_R32_SFLOAT, {uint32_t(W), uint32_t(H)}, u, true);
    AllocatedImage imCoarse = dev.createImage(VK_FORMAT_R32G32_SFLOAT, {1, 1}, u, true);  // unused (hasCoarse = 0)
    AllocatedImage imFlow = dev.createImage(VK_FORMAT_R32G32_SFLOAT, {uint32_t(bw), uint32_t(bh)}, u, true);
    AllocatedImage imCost = dev.createImage(VK_FORMAT_R32_SFLOAT, {uint32_t(bw), uint32_t(bh)}, u, true);
    const VkDeviceSize bytes = VkDeviceSize(W) * H * 4;
    AllocatedBuffer stage = dev.createBuffer(bytes * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    void* map = nullptr;
    check(vkMapMemory(device, stage.memory, 0, VK_WHOLE_SIZE, 0, &map), "vkMapMemory");
    memcpy(map, ref.v.data(), bytes);
    memcpy(static_cast<char*>(map) + bytes, tgt.v.data(), bytes);

    // Pipeline for block_match (f32), bindings as in FrameGen::createPipelines.
    const VkDescriptorType SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    const VkDescriptorType types[6] = {SI, SI, CIS, SI, SI, CIS};
    VkDescriptorSetLayoutBinding b[6];
    for (uint32_t i = 0; i < 6; ++i) b[i] = {i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 6;
    dl.pBindings = b;
    VkDescriptorSetLayout setLayout;
    check(vkCreateDescriptorSetLayout(device, &dl, nullptr, &setLayout), "set layout");
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(MatchPC)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VkPipelineLayout layout;
    check(vkCreatePipelineLayout(device, &pli, nullptr, &layout), "pipeline layout");
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = block_match_comp_f32_size;
    smi.pCode = block_match_comp_f32;
    VkShaderModule mod;
    check(vkCreateShaderModule(device, &smi, nullptr, &mod), "shader module");
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
    cpi.layout = layout;
    VkPipeline pipeline;
    check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline), "pipeline");

    VkDescriptorPoolSize ps[2] = {{SI, 4}, {CIS, 2}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = ps;
    VkDescriptorPool pool;
    check(vkCreateDescriptorPool(device, &dpi, nullptr, &pool), "descriptor pool");
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = pool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &setLayout;
    VkDescriptorSet set;
    check(vkAllocateDescriptorSets(device, &dai, &set), "descriptor set");
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkSampler sampler;
    check(vkCreateSampler(device, &sci, nullptr, &sampler), "sampler");
    const VkImageView views[6] = {imRef.view, imTgt.view, imCoarse.view, imFlow.view, imCost.view, imCoarse.view};
    VkDescriptorImageInfo ii[6];
    VkWriteDescriptorSet w[6];
    for (uint32_t i = 0; i < 6; ++i) {
        ii[i] = {sampler, views[i], VK_IMAGE_LAYOUT_GENERAL};
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = types[i];
        w[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(device, 6, w, 0, nullptr);

    MatchPC pc{{W, H}, {bw, bh}, R, 0, 0.f, 0.f, 0.f, 0.f, 0.05f, 0.f, 0, 0.f};
    dev.immediate([&](VkCommandBuffer cmd) {
        std::vector<VkImageMemoryBarrier> bars;
        for (const AllocatedImage* im : {&imRef, &imTgt, &imCoarse, &imFlow, &imCost})
            bars.push_back(imageBarrier(im->image, 0, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL));
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, uint32_t(bars.size()), bars.data());
        VkBufferImageCopy rc{};
        rc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rc.imageExtent = {uint32_t(W), uint32_t(H), 1};
        vkCmdCopyBufferToImage(cmd, stage.buffer, imRef.image, VK_IMAGE_LAYOUT_GENERAL, 1, &rc);
        rc.bufferOffset = bytes;
        vkCmdCopyBufferToImage(cmd, stage.buffer, imTgt.image, VK_IMAGE_LAYOUT_GENERAL, 1, &rc);
        VkClearColorValue zero{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, imCoarse.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                             nullptr, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(cmd, uint32_t(bw), uint32_t(bh), 1);
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
                             nullptr, 0, nullptr);
        VkBufferImageCopy fc{};
        fc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        fc.imageExtent = {uint32_t(bw), uint32_t(bh), 1};
        vkCmdCopyImageToBuffer(cmd, imFlow.image, VK_IMAGE_LAYOUT_GENERAL, stage.buffer, 1, &fc);
    });

    const float* flow = static_cast<const float*>(map);
    int mismatches = 0;
    for (int blk = 0; blk < bw * bh; ++blk) {
        const int e = expect[blk];
        const int ex = e % (2 * R + 1) - R, ey = e / (2 * R + 1) - R;
        const float gx = flow[blk * 2], gy = flow[blk * 2 + 1];
        if (gx != float(ex) || gy != float(ey)) {
            if (++mismatches <= 10)
                fprintf(stderr, "block (%d,%d): shader (%g,%g), reference (%d,%d)\n", blk % bw, blk / bw, gx, gy, ex, ey);
        }
    }
    printf("block matcher: %d blocks, %d where tree and sequential selection differ, %d mismatches\n", bw * bh,
           discriminating, mismatches);

    vkDeviceWaitIdle(device);
    vkUnmapMemory(device, stage.memory);
    dev.destroyBuffer(stage);
    for (AllocatedImage* im : {&imRef, &imTgt, &imCoarse, &imFlow, &imCost}) dev.destroyImage(*im);
    vkDestroySampler(device, sampler, nullptr);
    vkDestroyDescriptorPool(device, pool, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyShaderModule(device, mod, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr);
    vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    vkDestroyCommandPool(device, dev.cmdPool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return mismatches == 0 ? 0 : 1;
}
