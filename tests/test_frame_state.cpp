// Exercise the actual timing shader and its shared push-constant layout.
// In particular, phase smoothing must change interpolation t while keeping
// the source interval and duplicate/prediction clocks intact.
#include "device.h"
#include "framegen_internal.h"
#include "pacing.h"
#include "shaders.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace bdex;

namespace {
void check(VkResult r) {
    if (r != VK_SUCCESS) { std::fprintf(stderr, "frame state: Vulkan error %d\n", int(r)); std::exit(1); }
}

// The acceleration map must not turn constant velocity into curved motion
// just because two source intervals differ. Constant fields make the
// expected physical result analytic, independent of the matcher.
int testAccelTiming(DeviceData& dev, VkSampler sampler) {
    constexpr uint32_t W = 64, H = 64;
    constexpr VkDeviceSize bytes = W * H * 2 * sizeof(float);
    AllocatedImage images[4];
    for (auto& img : images)
        img = dev.createImage(VK_FORMAT_R32G32_SFLOAT, {W, H}, VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, true);
    auto readback = dev.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    void* mapped = nullptr;
    check(vkMapMemory(dev.device, readback.memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    VkDescriptorSetLayoutBinding bindings[4]{};
    for (uint32_t i = 0; i < 4; ++i)
        bindings[i] = {i, i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                       1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.bindingCount = 4; sli.pBindings = bindings;
    VkDescriptorSetLayout setLayout;
    check(vkCreateDescriptorSetLayout(dev.device, &sli, nullptr, &setLayout));
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(fg::AccelMapPC)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1; li.pSetLayouts = &setLayout; li.pushConstantRangeCount = 1; li.pPushConstantRanges = &range;
    VkPipelineLayout layout;
    check(vkCreatePipelineLayout(dev.device, &li, nullptr, &layout));
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = accel_map_comp_size; mi.pCode = accel_map_comp;
    VkShaderModule module;
    check(vkCreateShaderModule(dev.device, &mi, nullptr, &module));
    VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
    pi.layout = layout;
    VkPipeline pipeline;
    check(vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline));
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1; dpi.poolSizeCount = 2; dpi.pPoolSizes = sizes;
    VkDescriptorPool pool;
    check(vkCreateDescriptorPool(dev.device, &dpi, nullptr, &pool));
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &setLayout;
    VkDescriptorSet set;
    check(vkAllocateDescriptorSets(dev.device, &ai, &set));
    VkDescriptorImageInfo imageInfos[4];
    VkWriteDescriptorSet writes[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        imageInfos[i] = {sampler, images[i].view, VK_IMAGE_LAYOUT_GENERAL};
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set; writes[i].dstBinding = i; writes[i].descriptorCount = 1;
        writes[i].descriptorType = bindings[i].descriptorType; writes[i].pImageInfo = &imageInfos[i];
    }
    vkUpdateDescriptorSets(dev.device, 4, writes, 0, nullptr);
    fg::AccelMapPC pc{{W, H}, {W * 4, H * 4}, {1.f / (W * 4), 1.f / (H * 4)}, 1.f, 1.f};
    int failures = 0;
    struct Case { float current, previous, ratio, expected; };
    const Case cases[] = {{16, 8, 2, 0}, {8, 16, .5f, 0}, {16, 16, 2, 0},
                          {16, 8, 1, 8}, {27, 12, 1.5f, 10.8f}, {12, 18, .5f, 0},
                          {20, 8, 2, 16.f / 3}, {20, 12, .5f, 8}, {16, 8, 0, 0},
                          {80, 8, 10, 0}, {8, 80, .1f, 0}, {17, 8, 2, 0}};
    for (const Case& c : cases) {
        pc.intervalRatio = c.ratio;
        dev.immediate([&](VkCommandBuffer cmd) {
            const VkImageSubresourceRange sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            for (int i = 0; i < 4; ++i) {
                auto b = imageBarrier(images[i].image, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                       VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &b);
                VkClearColorValue value{};
                value.float32[0] = i == 0 ? c.current : i == 1 ? -c.current : i == 2 ? -c.previous : 0.f;
                vkCmdClearColorImage(cmd, images[i].image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &sub);
                b = imageBarrier(images[i].image, VK_ACCESS_TRANSFER_WRITE_BIT,
                                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                  VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &b);
            }
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            vkCmdDispatch(cmd, W / 16, H / 16, 1);
            auto b = imageBarrier(images[3].image, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                   VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {W, H, 1};
            vkCmdCopyImageToBuffer(cmd, images[3].image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &copy);
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                 0, 1, &mb, 0, nullptr, 0, nullptr);
        });
        const float* v = static_cast<const float*>(mapped) + 2 * ((H / 2) * W + W / 2);
        if (std::abs(v[0] - c.expected) > 1e-3f || std::abs(v[1]) > 1e-3f) {
            std::fprintf(stderr, "accel timing: current %.1f prev %.1f ratio %.2f: (%f,%f), expected %f\n",
                         c.current, c.previous, c.ratio, v[0], v[1], c.expected);
            ++failures;
        }
    }
    vkUnmapMemory(dev.device, readback.memory); dev.destroyBuffer(readback);
    for (auto& img : images) dev.destroyImage(img);
    vkDestroyDescriptorPool(dev.device, pool, nullptr); vkDestroyPipeline(dev.device, pipeline, nullptr);
    vkDestroyShaderModule(dev.device, module, nullptr); vkDestroyPipelineLayout(dev.device, layout, nullptr);
    vkDestroyDescriptorSetLayout(dev.device, setLayout, nullptr);
    std::printf("accel timing: %d failures\n", failures);
    return failures;
}
}

int main() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) return 77;
    uint32_t n = 0;
    check(vkEnumeratePhysicalDevices(instance, &n, nullptr));
    if (!n) { vkDestroyInstance(instance, nullptr); return 77; }
    std::vector<VkPhysicalDevice> physical(n);
    check(vkEnumeratePhysicalDevices(instance, &n, physical.data()));
    const VkPhysicalDevice pd = physical[0];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, nullptr);
    std::vector<VkQueueFamilyProperties> families(n);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, families.data());
    uint32_t family = 0;
    while (family < n && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
    if (family == n) { vkDestroyInstance(instance, nullptr); return 77; }
    const float priority = 1.f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family; qi.queueCount = 1; qi.pQueuePriorities = &priority;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
    VkDevice device;
    check(vkCreateDevice(pd, &di, nullptr, &device));
    InstanceData inst;
    inst.instance = instance; inst.vt.load(instance, vkGetInstanceProcAddr);
    DeviceData dev;
    dev.inst = &inst; dev.device = device; dev.physDev = pd; dev.queueFamily = family;
    dev.vt.load(device, vkGetDeviceProcAddr);
    vkGetPhysicalDeviceProperties(pd, &dev.props);
    vkGetPhysicalDeviceMemoryProperties(pd, &dev.memProps);
    vkGetDeviceQueue(device, family, 0, &dev.queue);
    VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ci.queueFamilyIndex = family;
    check(vkCreateCommandPool(device, &ci, nullptr, &dev.cmdPool));

    auto state = dev.createBuffer(fg::kStateSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    void* mapped = nullptr;
    check(vkMapMemory(device, state.memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    auto luma = dev.createImage(VK_FORMAT_R32_SFLOAT, {1, 1}, VK_IMAGE_USAGE_SAMPLED_BIT, true);
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkSampler sampler;
    check(vkCreateSampler(device, &sci, nullptr, &sampler));
    const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    VkDescriptorSetLayoutBinding bindings[3] = {
        {0, CIS, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, CIS, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, SB, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.bindingCount = 3; sl.pBindings = bindings;
    VkDescriptorSetLayout setLayout;
    check(vkCreateDescriptorSetLayout(device, &sl, nullptr, &setLayout));
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(fg::FrameStatePC)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1; li.pSetLayouts = &setLayout; li.pushConstantRangeCount = 1; li.pPushConstantRanges = &range;
    VkPipelineLayout layout;
    check(vkCreatePipelineLayout(device, &li, nullptr, &layout));
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = frame_state_comp_size; mi.pCode = frame_state_comp;
    VkShaderModule module;
    check(vkCreateShaderModule(device, &mi, nullptr, &module));
    VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
    pi.layout = layout;
    VkPipeline pipeline;
    check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline));
    VkDescriptorPoolSize sizes[2] = {{CIS, 2}, {SB, 1}};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1; poolInfo.poolSizeCount = 2; poolInfo.pPoolSizes = sizes;
    VkDescriptorPool pool;
    check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool));
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &setLayout;
    VkDescriptorSet set;
    check(vkAllocateDescriptorSets(device, &ai, &set));
    VkDescriptorImageInfo imageInfo{sampler, luma.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bufferInfo{state.buffer, 0, fg::kStateSize};
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set; writes[i].dstBinding = i; writes[i].descriptorCount = 1;
        writes[i].descriptorType = i < 2 ? CIS : SB;
        if (i < 2) writes[i].pImageInfo = &imageInfo;
        else writes[i].pBufferInfo = &bufferInfo;
    }
    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
    dev.immediate([&](VkCommandBuffer cmd) {
        auto b = imageBarrier(luma.image, 0, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
    });

    const float P = 1000.f / 144.f;
    fg::FrameStatePC pc{{1, 1}, 1, 4, 1000.f / 30.f, 1000.f / 30.f,
                        fg::kStateDisplay | fg::kStateContent, 0, 0, {}, 3, 4 * P};
    pc.displayMs[0] = 0.f; pc.displayMs[1] = P; pc.displayMs[2] = 2 * P; pc.displayMs[7] = 3 * P;
    int failures = 0;
    auto run = [&](bool duplicate, float expectedFirst) {
        std::memset(mapped, 0, size_t(fg::kStateSize));
        static_cast<float*>(mapped)[4] = pc.intervalMs;  // the previous distinct image's age
        dev.immediate([&](VkCommandBuffer cmd) {
            VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            b.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            vkCmdDispatch(cmd, 1, 1, 1);
            b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
        });
        const auto* f = static_cast<const float*>(mapped);
        const auto* u = static_cast<const uint32_t*>(mapped);
        if (std::abs(f[8] - expectedFirst) > 1e-5f || (u[1] != 0) != duplicate ||
            std::abs(f[5] - pc.intervalMs) > 1e-4f) {
            std::fprintf(stderr, "frame state: t=%f expected=%f dup=%u interval=%f\n", f[8], expectedFirst, u[1], f[5]);
            ++failures;
        }
    };
    run(false, 0.25f);  // four display periods for the pair, source interval stays 33.33 ms
    pc.multiplier = 3; pc.realPos = 2; pc.displayMs[7] = 2 * P; pc.displaySpanMs = 3 * P;
    run(false, 1.f / 3.f); // gradual catch-up: the shorter job keeps intermediate motion
    pc.multiplier = 4; pc.realPos = 3; pc.displayMs[7] = 3 * P;
    pc.displaySpanMs = 0.f;
    run(false, 0.375f); // off: original content-interval denominator
    pc.displaySpanMs = 4 * P; pc.flags |= fg::kStateDedup;
    pc.sinceLastMs = 5.f;
    run(true, 0.525f);  // duplicate detection overrides smoothing, keeps the distinct pair's interval
    pc.flags &= ~fg::kStateDedup; pc.realPos = 0; pc.sinceLastMs = pc.intervalMs;
    run(false, 1.f);    // predictions keep their source clock
    // The generalized schedule also supplies shared-refresh and sub-refresh
    // plans. Exercise every output slot against analytic interpolation times
    // at arbitrary rates, on both sides of the output/source Hz boundaries.
    const pacing::Grid grid{true, 0.0, 1000.0 / 144};
    for (double fps : {23.976, 35.9, 36.2, 59.94, 90.0, 200.0}) {
        for (int m = 2; m <= 8; ++m) {
            const double interval = 1000.0 / fps;
            double base[8] = {}, planned[8] = {}, last = 0.0;
            for (int k = 0; k < m; ++k) base[k] = interval * k / m;
            pacing::planLands(1000.0, 1.0, base, m, grid, last, planned, interval, m - 1);
            pc.flags = fg::kStateDisplay | fg::kStateContent;
            pc.multiplier = m; pc.realPos = m - 1;
            pc.sinceLastMs = pc.intervalMs = pc.displaySpanMs = float(interval);
            for (int o = 0; o < m - 1; ++o) pc.displayMs[o] = float(planned[o] - 1000.0);
            pc.displayMs[7] = float(planned[m - 1] - 1000.0);
            const auto expected = [&](int o) {
                return std::clamp(1.0 + (planned[o] - planned[m - 1]) / interval, 0.0, 1.0);
            };
            run(false, float(expected(0)));
            for (int o = 0; o < m - 1; ++o) {
                const float t = static_cast<const float*>(mapped)[8 + o];
                if (!std::isfinite(t) || std::abs(t - expected(o)) > 1e-5) {
                    std::fprintf(stderr, "frame state: %.3f fps x%d slot %d: t=%f expected=%f\n",
                                 fps, m, o, t, expected(o));
                    ++failures;
                }
            }
        }
    }
    std::printf("frame state: %d failures\n", failures);
    failures += testAccelTiming(dev, sampler);

    vkUnmapMemory(device, state.memory);
    dev.destroyBuffer(state); dev.destroyImage(luma);
    vkDestroySampler(device, sampler, nullptr); vkDestroyDescriptorPool(device, pool, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr); vkDestroyShaderModule(device, module, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr); vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    vkDestroyCommandPool(device, dev.cmdPool, nullptr); vkDestroyDevice(device, nullptr); vkDestroyInstance(instance, nullptr);
    return failures ? 1 : 0;
}
