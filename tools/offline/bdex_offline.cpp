// bdex_offline — runs the frame-generation engine (FrameGen, unchanged) on a
// sequence of PPM frames without any window, surface or layer.
//
// The input is a directory of consecutive frames captured at a high frame
// rate (e.g. a game dumped through the layer with BDEX_FG_MULTIPLIER=1 and
// BDEX_FG_DUMP=<dir>). Every `multiplier`-th frame is fed to the engine as a
// "real" frame; the frames in between are generated and written next to the
// ground truth they should match, so tools/eval_sequence.py can score them.
// Because no window is involved, the result does not depend on the size a
// compositor gives the window, and any recorded game becomes a test scene.
//
// Usage: bdex_offline <in_dir> <out_dir> [--multiplier N] [--srgb] [--gpu I] [--max-inputs N]
// Settings come from BDEX_FG_* environment variables (the config file is
// ignored unless BDEX_FG_CONFIG names one), e.g. BDEX_FG_MODE=extrapolate.
//
// Output: <out_dir>/gen_<k>_<i>.ppm, the frame generated for input pair k at
// position i (1..multiplier-1), plus manifest.txt with one line per generated
// frame: "<generated file> <ground-truth file> <t> <previous input> <current input>".
#include "config.h"
#include "device.h"
#include "framegen.h"
#include "log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace bdex;

namespace {

struct Image {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgb;
};

bool readPpm(const fs::path& path, Image& img) {
    FILE* f = fopen(path.string().c_str(), "rb");
    if (!f) return false;
    char magic[3] = {};
    unsigned w = 0, h = 0, maxv = 0;
    bool ok = fscanf(f, "%2s", magic) == 1 && strcmp(magic, "P6") == 0;
    // Skip comments between header fields.
    auto field = [&](unsigned& v) {
        int c;
        while ((c = fgetc(f)) != EOF) {
            if (c == '#') { while ((c = fgetc(f)) != EOF && c != '\n') {} continue; }
            if (!isspace(c)) { ungetc(c, f); break; }
        }
        return fscanf(f, "%u", &v) == 1;
    };
    ok = ok && field(w) && field(h) && field(maxv) && maxv == 255 && w > 0 && h > 0;
    if (ok) {
        fgetc(f);  // the single whitespace after maxval
        img.w = w;
        img.h = h;
        img.rgb.resize(size_t(w) * h * 3);
        ok = fread(img.rgb.data(), 1, img.rgb.size(), f) == img.rgb.size();
    }
    fclose(f);
    return ok;
}

[[noreturn]] void die(const char* msg) {
    fprintf(stderr, "bdex_offline: %s\n", msg);
    exit(1);
}

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        fprintf(stderr, "bdex_offline: %s failed (%d)\n", what, static_cast<int>(r));
        exit(1);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <in_dir> <out_dir> [--multiplier N] [--srgb] [--gpu I] [--max-inputs N]\n", argv[0]);
        return 2;
    }
    const fs::path inDir = argv[1], outDir = argv[2];
    int mult = 2, gpu = -1, maxInputs = 0;
    bool srgb = false;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--multiplier" && i + 1 < argc) mult = atoi(argv[++i]);
        else if (a == "--gpu" && i + 1 < argc) gpu = atoi(argv[++i]);
        else if (a == "--max-inputs" && i + 1 < argc) maxInputs = atoi(argv[++i]);
        else if (a == "--srgb") srgb = true;
        else { fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    if (mult < 2 || mult > 4) die("--multiplier must be 2, 3 or 4");

    std::vector<fs::path> frames;
    for (auto& e : fs::directory_iterator(inDir))
        if (e.path().extension() == ".ppm") frames.push_back(e.path());
    std::sort(frames.begin(), frames.end());
    if (frames.size() < size_t(mult) + 1) die("not enough .ppm frames in the input directory");
    Image first;
    if (!readPpm(frames[0], first)) die("cannot read the first frame");
    const VkExtent2D extent{first.w, first.h};

    // This process drives FrameGen itself: never load the implicit layer, and
    // do not inherit the user's game profiles unless explicitly asked to.
    setenv("BDEX_FG_DISABLE", "1", 1);
    if (!getenv("BDEX_FG_CONFIG")) setenv("BDEX_FG_CONFIG", "/dev/null", 1);
    Config cfg = Config::load();
    cfg.enabled = true;
    cfg.multiplier = mult;
    cfg.overlayMode = 0;
    log_set_level(cfg.logLevel);

    // Headless Vulkan: no surface extension. VK_KHR_swapchain is enabled on
    // the device only because FrameGen expects its sources in PRESENT_SRC.
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "bdex_offline";
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
    VkPhysicalDevice pd = pds[0];
    if (gpu >= 0) {
        if (size_t(gpu) >= pds.size()) die("--gpu index out of range");
        pd = pds[gpu];
    } else {
        // Same preference as the capture app: discrete > integrated > other.
        auto score = [](VkPhysicalDevice p) {
            VkPhysicalDeviceProperties pr;
            vkGetPhysicalDeviceProperties(p, &pr);
            return pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                   : pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                                                                             : 0;
        };
        for (auto p : pds)
            if (score(p) > score(pd)) pd = p;
    }
    uint32_t nQf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nQf, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(nQf);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nQf, qfs.data());
    uint32_t qf = UINT32_MAX;
    for (uint32_t i = 0; i < nQf; ++i)
        if (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qf = i; break; }
    if (qf == UINT32_MAX) die("no compute queue");

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

    // The layer's DeviceData over a plain device, as in capture/src/vkctx.cpp.
    InstanceData inst;
    inst.vt.load(instance, vkGetInstanceProcAddr);
    inst.instance = instance;
    inst.apiVersion = VK_API_VERSION_1_1;
    inst.config = cfg;
    DeviceData dev;
    dev.vt.load(device, vkGetDeviceProcAddr);
    dev.inst = &inst;
    dev.physDev = pd;
    dev.device = device;
    dev.config = cfg;
    vkGetPhysicalDeviceProperties(pd, &dev.props);
    vkGetPhysicalDeviceMemoryProperties(pd, &dev.memProps);
    vkGetDeviceQueue(device, qf, 0, &dev.queue);
    dev.queueFamily = qf;
    dev.layerUsable = true;
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = qf;
    check(vkCreateCommandPool(device, &cp, nullptr, &dev.cmdPool), "vkCreateCommandPool");
    dev.config.autoConfigure(dev.props, dev.memProps);
    fprintf(stderr, "bdex_offline: %s, %ux%u, x%d %s, %s\n", dev.props.deviceName, extent.width, extent.height,
            mult, dev.config.extrapolate ? "extrapolate" : "interpolate", dev.config.describe().c_str());

    const VkFormat format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    int status = 0;
    {
        // Two history images play the role of the game's swapchain images.
        std::vector<AllocatedImage> hist(2);
        std::vector<VkImage> sources;
        for (auto& h : hist) {
            h = dev.createImage(format, extent,
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT,
                                false);
            sources.push_back(h.image);
        }
        AllocatedBuffer staging = dev.createBuffer(VkDeviceSize(extent.width) * extent.height * 4,
                                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        void* mapped = nullptr;
        check(vkMapMemory(device, staging.memory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
        auto fgp = std::make_unique<FrameGen>(dev, format, extent, extent, static_cast<uint32_t>(mult - 1), sources);
        FrameGen& fg = *fgp;

        fs::create_directories(outDir);
        std::ofstream manifest(outDir / "manifest.txt");
        const bool extrap = dev.config.extrapolate;
        // Input k is frame k*mult. Interpolation generates frames strictly
        // between inputs k-1 and k; extrapolation generates the frames after
        // input k (predicted from k-1 and k), so its last input needs mult-1
        // ground-truth frames after it.
        size_t nInputs = (frames.size() - 1) / mult + 1;
        if (extrap) nInputs = (frames.size() - mult) / mult + 1;
        if (maxInputs > 0) nInputs = std::min(nInputs, size_t(maxInputs));

        int prev = -1;
        uint32_t parity = 0;
        Image img;
        for (size_t k = 0; k < nInputs; ++k) {
            const size_t fi = k * mult;
            if (!readPpm(frames[fi], img) || img.w != extent.width || img.h != extent.height) {
                fprintf(stderr, "bdex_offline: %s: unreadable or size differs from the first frame\n",
                        frames[fi].string().c_str());
                status = 1;
                break;
            }
            auto* dst = static_cast<uint8_t*>(mapped);
            for (size_t p = 0, n = size_t(img.w) * img.h; p < n; ++p) {
                dst[p * 4 + 0] = img.rgb[p * 3 + 0];
                dst[p * 4 + 1] = img.rgb[p * 3 + 1];
                dst[p * 4 + 2] = img.rgb[p * 3 + 2];
                dst[p * 4 + 3] = 255;
            }
            const int cur = static_cast<int>(k % 2);
            const bool generate = prev >= 0;
            dev.immediate([&](VkCommandBuffer cmd) {
                // Upload into the history image and hand it over the way a
                // game does: in PRESENT_SRC.
                VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = hist[cur].image;
                b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                     nullptr, 0, nullptr, 1, &b);
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {extent.width, extent.height, 1};
                vkCmdCopyBufferToImage(cmd, staging.buffer, hist[cur].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                       &region);
                b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                     nullptr, 0, nullptr, 1, &b);

                // The same sequence as VirtualSwapchain::present.
                fg.recordAcquireSources(cmd, prev, cur);
                fg.recordAnalysis(cmd, parity, static_cast<uint32_t>(cur));
                if (generate) {
                    fg.recordFlow(cmd, parity);
                    const float base = extrap ? 1.f : 0.f;
                    for (int i = 1; i < mult; ++i) {
                        fg.recordInterpolate(cmd, static_cast<uint32_t>(prev), static_cast<uint32_t>(cur),
                                             base + float(i) / mult, static_cast<uint32_t>(i - 1));
                        fg.recordDump(cmd, static_cast<uint32_t>(i - 1), static_cast<uint32_t>(cur));
                    }
                }
                fg.recordReleaseSources(cmd, prev, cur);
            });
            if (generate) {
                for (int i = 1; i < mult; ++i) {
                    // Interpolation between inputs k-1 and k; extrapolation after k.
                    const size_t gt = extrap ? fi + i : fi - mult + i;
                    char name[64];
                    snprintf(name, sizeof name, "gen_%04zu_%d.ppm", k, i);
                    if (!fg.writeDump((outDir / name).string(), static_cast<uint32_t>(i - 1))) {
                        fprintf(stderr, "bdex_offline: cannot write %s\n", name);
                        status = 1;
                    }
                    manifest << name << ' ' << fs::absolute(frames[gt]).string() << ' '
                             << (extrap ? 1.f : 0.f) + float(i) / mult << ' '
                             << fs::absolute(frames[fi - mult]).string() << ' '
                             << fs::absolute(frames[fi]).string() << '\n';
                }
            }
            prev = cur;
            parity ^= 1;
        }
        vkDeviceWaitIdle(device);
        vkUnmapMemory(device, staging.memory);
        dev.destroyBuffer(staging);
        fgp.reset();  // the engine holds views of the history images
        for (auto& h : hist) dev.destroyImage(h);
    }
    vkDestroyCommandPool(device, dev.cmdPool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return status;
}
