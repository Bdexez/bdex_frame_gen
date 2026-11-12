// bdex-calibrate — what frame generation costs on this GPU, before any game:
// the engine (FrameGen, unchanged) on synthetic moving frames at the
// display's size, headless, for the automatic preset and each fixed one; the
// mean GPU time of the analysis + flow (once per game frame) and of one
// synthesis (per generated frame), then a recommendation — the best preset
// and the highest multiplier whose cost stays within a quarter of the GPU's
// time at the display's refresh rate (the rest for the game).
//
// Usage: bdex-calibrate [--size WxH] [--refresh HZ] [--gpu NAME] [--frames N] [--json FILE]
// Run by the control panel and the Windows launcher on first start
// (« Calibrer »), and by `bdex-framegen --calibrate [--apply]`.
#include "platform.h"
#include "config.h"
#include "device.h"
#include "framegen.h"
#include "frame_order.h"
#include "log.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace bdex;

namespace {

[[noreturn]] void die(const char* msg) {
    fprintf(stderr, "bdex-calibrate: %s\n", msg);
    exit(1);
}

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        fprintf(stderr, "bdex-calibrate: %s failed (%d)\n", what, static_cast<int>(r));
        exit(1);
    }
}

void setEnv(const char* k, const char* v) {
#ifdef _WIN32
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
    bdex::platformEnvForget();  // (the engine reads a copy of the environment)
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A textured picture (value noise at two scales, stripes, a few disks): the
// matcher and the synthesis do the work of real content on it.
std::vector<uint8_t> makeTexture(uint32_t w, uint32_t h) {
    std::vector<uint8_t> t(size_t(w) * h * 4);
    uint32_t rng = 12345;
    auto rnd = [&] { rng = rng * 1664525u + 1013904223u; return (rng >> 24) & 255u; };
    const uint32_t gw = w / 16 + 2, gh = h / 16 + 2;
    std::vector<uint8_t> grid(size_t(gw) * gh * 3);
    for (auto& g : grid) g = static_cast<uint8_t>(rnd());
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const float fx = x / 16.f, fy = y / 16.f;
            const uint32_t ix = uint32_t(fx), iy = uint32_t(fy);
            const float ax = fx - ix, ay = fy - iy;
            uint8_t* p = &t[(size_t(y) * w + x) * 4];
            for (int c = 0; c < 3; ++c) {
                auto g = [&](uint32_t gx, uint32_t gy) { return float(grid[(size_t(gy) * gw + gx) * 3 + c]); };
                float v = (g(ix, iy) * (1 - ax) + g(ix + 1, iy) * ax) * (1 - ay) + (g(ix, iy + 1) * (1 - ax) + g(ix + 1, iy + 1) * ax) * ay;
                v = v * 0.7f + 0.3f * (((x / 7 + y / 11 + c) % 5) * 50.f);
                p[c] = static_cast<uint8_t>(std::clamp(v, 0.f, 255.f));
            }
            p[3] = 255;
        }
    return t;
}

struct Result {
    std::string name, resolved;
    double analysis = 0, flow = 0, synth = 0;  // ms
    double perGameFrame(int m) const { return analysis + flow + (m - 1) * synth; }
};

} // namespace

int main(int argc, char** argv) {
    uint32_t w = 1920, h = 1080;
    double refresh = 60.0;
    int frames = 30;
    std::string gpuName, jsonPath;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--size" && i + 1 < argc) {
            if (sscanf(argv[++i], "%ux%u", &w, &h) != 2) die("--size WxH");
        } else if (a == "--refresh" && i + 1 < argc) refresh = atof(argv[++i]);
        else if (a == "--gpu" && i + 1 < argc) gpuName = argv[++i];
        else if (a == "--frames" && i + 1 < argc) frames = std::max(5, atoi(argv[++i]));
        else if (a == "--json" && i + 1 < argc) jsonPath = argv[++i];
        else {
            fprintf(stderr, "usage: bdex-calibrate [--size WxH] [--refresh HZ] [--gpu NAME] [--frames N] [--json FILE]\n");
            return 2;
        }
    }
    if (w < 64 || h < 64 || w > 8192 || h > 8192) die("--size out of range");
    if (!(refresh >= 20.0 && refresh <= 1000.0)) refresh = 60.0;

    // This process drives FrameGen itself: never the implicit layer, never the
    // user's settings (the defaults are what is measured).
    setEnv("BDEX_FG_DISABLE", "1");
#ifdef _WIN32
    setEnv("BDEX_FG_CONFIG", "NUL");
#else
    setEnv("BDEX_FG_CONFIG", "/dev/null");
#endif

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "bdex-calibrate";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance;
    check(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");
    uint32_t nPd = 0;
    vkEnumeratePhysicalDevices(instance, &nPd, nullptr);
    std::vector<VkPhysicalDevice> pds(nPd);
    vkEnumeratePhysicalDevices(instance, &nPd, pds.data());
    if (pds.empty()) die("no Vulkan device");
    // The GPU asked for (a part of its name), else the one games use: discrete > integrated.
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    int best = -1;
    for (auto p : pds) {
        VkPhysicalDeviceProperties pr;
        vkGetPhysicalDeviceProperties(p, &pr);
        int score = pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 0;
        if (!gpuName.empty()) score = lower(pr.deviceName).find(lower(gpuName)) != std::string::npos ? 10 : -1;
        if (score > best) { best = score; pd = p; }
    }
    if (best < 0) die("no GPU of that name");
    uint32_t nQf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nQf, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(nQf);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nQf, qfs.data());
    uint32_t qf = UINT32_MAX;
    for (uint32_t i = 0; i < nQf; ++i)
        if ((qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && qfs[i].timestampValidBits) { qf = i; break; }
    if (qf == UINT32_MAX) die("no compute queue with timestamps");
    float prio = 1.f;
    VkDeviceQueueCreateInfo dq{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    dq.queueFamilyIndex = qf;
    dq.queueCount = 1;
    dq.pQueuePriorities = &prio;
    const char* devExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &dq;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = devExts;
    VkPhysicalDeviceFeatures feats{};
    dci.pEnabledFeatures = &feats;
    VkDevice device;
    check(vkCreateDevice(pd, &dci, nullptr, &device), "vkCreateDevice");

    InstanceData inst;
    inst.vt.load(instance, vkGetInstanceProcAddr);
    inst.instance = instance;
    inst.apiVersion = VK_API_VERSION_1_1;
    DeviceData dev;
    dev.vt.load(device, vkGetDeviceProcAddr);
    dev.inst = &inst;
    dev.physDev = pd;
    dev.device = device;
    vkGetPhysicalDeviceProperties(pd, &dev.props);
    vkGetPhysicalDeviceMemoryProperties(pd, &dev.memProps);
    vkGetDeviceQueue(device, qf, 0, &dev.queue);
    dev.queueFamily = qf;
    dev.layerUsable = true;
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = qf;
    check(vkCreateCommandPool(device, &cp, nullptr, &dev.cmdPool), "vkCreateCommandPool");

    const VkExtent2D extent{w, h};
    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    std::vector<AllocatedImage> hist(2);
    std::vector<VkImage> sources;
    for (auto& img : hist) {
        img = dev.createImage(format, extent, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, false);
        sources.push_back(img.image);
    }
    const VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
    AllocatedBuffer staging = dev.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    void* mapped = nullptr;
    check(vkMapMemory(device, staging.memory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
    const std::vector<uint8_t> tex = makeTexture(w, h);

    std::vector<Result> results;
    for (const char* preset : {"auto", "performance", "balanced", "quality"}) {
        Config cfg = Config::load();
        cfg.apply("preset", preset);
        cfg.enabled = true;
        cfg.multiplier = 2;
        cfg.overlayMode = 0;
        cfg.profile = true;
        cfg.onBattery = false;  // (the cost of the GPU's own defaults)
        cfg.autoConfigure(dev.props, dev.memProps);
        dev.config = cfg;
        inst.config = cfg;
        Result r;
        r.name = preset;
        r.resolved = std::string(preset) == "auto" ? cfg.autoTunedTo : preset;
        {
            auto fg = std::make_unique<FrameGen>(dev, format, extent, extent, 1u, sources, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
            const FrameOrder order = FrameOrder::of(dev.config, 2);
            int prev = -1;
            uint32_t parity = 0;
            const int warmup = 8;
            for (int k = 0; k < warmup + frames; ++k) {
                if (k == warmup) {
                    double ms[FrameGen::StageCount];
                    fg->profileTake(ms);  // (the warm-up does not count: clocks ramping, pipelines compiling)
                }
                // The picture panned (8 px right, 3 px down a frame): real motion for the flow.
                const uint32_t dx = uint32_t(k * 8) % w, dy = uint32_t(k * 3) % h;
                uint8_t* out = static_cast<uint8_t*>(mapped);
                for (uint32_t y = 0; y < h; ++y) {
                    const uint32_t sy = (y + dy) % h;
                    const uint8_t* row = &tex[size_t(sy) * w * 4];
                    std::memcpy(out + size_t(y) * w * 4, row + size_t(dx) * 4, size_t(w - dx) * 4);
                    std::memcpy(out + size_t(y) * w * 4 + size_t(w - dx) * 4, row, size_t(dx) * 4);
                }
                const int cur = k % 2;
                FrameGen::FrameTiming timing;
                timing.multiplier = prev >= 0 ? 2 : 1;
                timing.reset = prev < 0;
                timing.sinceLastMs = timing.intervalMs = 2000.0 / refresh;
                dev.immediate([&](VkCommandBuffer cmd) {
                    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b.image = hist[size_t(cur)].image;
                    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
                    VkBufferImageCopy region{};
                    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    region.imageExtent = {w, h, 1};
                    vkCmdCopyBufferToImage(cmd, staging.buffer, hist[size_t(cur)].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
                    // The same sequence as VirtualSwapchain::present, x2.
                    fg->profileBegin(cmd, 0);
                    fg->recordAcquireSources(cmd, prev, cur);
                    fg->recordAnalysis(cmd, parity, static_cast<uint32_t>(cur));
                    fg->recordFrameState(cmd, timing);
                    fg->profileMark(cmd, 0, FrameGen::StageAnalysis);
                    if (prev >= 0) {
                        fg->recordFlow(cmd, parity);
                        fg->profileMark(cmd, 0, FrameGen::StageFlow);
                        fg->recordInterpolate(cmd, static_cast<uint32_t>(prev), static_cast<uint32_t>(cur), order.t(0), 0);
                        fg->profileMark(cmd, 0, FrameGen::StageInterp);
                    }
                    if (fg->packReal()) fg->recordUpscaleReal(cmd, static_cast<uint32_t>(cur), prev);
                    fg->recordReleaseSources(cmd, prev, cur);
                });
                fg->profileCollect(0);
                prev = cur;
                parity ^= 1;
            }
            double ms[FrameGen::StageCount] = {};
            if (!fg->profileTake(ms)) die("no GPU timestamps");
            r.analysis = ms[FrameGen::StageAnalysis];
            r.flow = ms[FrameGen::StageFlow];
            r.synth = ms[FrameGen::StageInterp];
            vkDeviceWaitIdle(device);
        }
        results.push_back(r);
        fprintf(stderr, "bdex-calibrate: %-11s (%s) analysis %.2f ms, flow %.2f ms, synthesis %.2f ms a generated frame\n",
                r.name.c_str(), r.resolved.c_str(), r.analysis, r.flow, r.synth);
    }

    // The recommendation: within a quarter of the GPU's time. A game at
    // refresh/m fps leaves m refresh periods per game frame; the generation
    // costs analysis + flow + (m-1) synthesis in it.
    const double kBudget = 0.25, period = 1000.0 / refresh;
    auto fits = [&](const Result& r, int m) { return r.perGameFrame(m) <= kBudget * m * period; };
    // The automatic preset (the GPU's own defaults, quality options included)
    // when it fits, else the lighter fixed ones; quality (full-resolution
    // flow) is measured but never preferred to auto: it costs far more for
    // what the automatic options already bring.
    const Result* pick = nullptr;
    for (const char* name : {"auto", "balanced", "performance"}) {
        for (const auto& r : results)
            if (r.name == name && fits(r, 2)) pick = &r;
        if (pick) break;
    }
    if (!pick) pick = &results[1];  // performance, even over budget: the lightest there is
    // The multiplier: within the budget, and no game below 30 fps (a frame
    // rate generation cannot make smooth: 60 Hz x2, 144 Hz x4, 240 Hz x8).
    const int byRate = std::max(2, std::min(8, int(refresh / 30.0 + 0.01)));
    int mult = 2;
    for (int m = 2; m <= byRate; ++m)
        if (fits(*pick, m)) mult = m;
    fprintf(stderr, "bdex-calibrate: %s, %ux%u at %.0f Hz: preset %s, up to x%d (x2 costs %.2f ms a game frame)\n",
            dev.props.deviceName, w, h, refresh, pick->name.c_str(), mult, pick->perGameFrame(2));

    std::string json = "{\n  \"gpu\": \"";
    for (char c : std::string(dev.props.deviceName)) json += (c == '"' || c == '\\') ? ' ' : c;
    char buf[512];
    snprintf(buf, sizeof buf, "\",\n  \"size\": [%u, %u],\n  \"refresh\": %.1f,\n  \"budget\": %.2f,\n  \"presets\": {\n", w, h, refresh, kBudget);
    json += buf;
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        snprintf(buf, sizeof buf, "    \"%s\": {\"resolved\": \"%s\", \"analysis_ms\": %.3f, \"flow_ms\": %.3f, \"synthesis_ms\": %.3f}%s\n",
                 r.name.c_str(), r.resolved.c_str(), r.analysis, r.flow, r.synth, i + 1 < results.size() ? "," : "");
        json += buf;
    }
    snprintf(buf, sizeof buf, "  },\n  \"recommended\": {\"preset\": \"%s\", \"multiplier\": %d, \"x2_ms\": %.3f, \"cost_ms\": %.3f}\n}\n",
             pick->name.c_str(), mult, pick->perGameFrame(2), pick->perGameFrame(mult));
    json += buf;
    if (!jsonPath.empty()) {
        std::ofstream out(jsonPath, std::ios::trunc);
        out << json;
    } else {
        fputs(json.c_str(), stdout);
    }

    vkUnmapMemory(device, staging.memory);
    dev.destroyBuffer(staging);
    for (auto& img : hist) dev.destroyImage(img);
    vkDestroyCommandPool(device, dev.cmdPool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return 0;
}
