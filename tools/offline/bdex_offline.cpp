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
// Usage: bdex_offline <in_dir> <out_dir> [--multiplier N] [--format F] [--colorspace C] [--render-scale S]
//                     [--gpu I] [--max-inputs N] [--jitter J] [--dup N] [--spacing S] [--seed N] [--verbose] [--score-real]
//   --format        swapchain format the engine sees: rgba8 (default), bgra8,
//                   srgb (R8G8B8A8_SRGB), rgb10 (A2B10G10R10), rgba16f —
//                   exercises every output encoding (--srgb = --format srgb)
//   --render-scale  the "game" renders at this fraction of the frames' size
//                   (inputs are downscaled on the CPU) and the engine
//                   upscales its output back to the frames' size
//   --colorspace    srgb (default), scrgb (FP16 linear, 1 = 80 nits; needs
//                   --format rgba16f) or hdr10 (PQ; --format rgb10): the SDR
//                   frames are converted with white at 203 nits, and the
//                   dumps converted back, so HDR runs score against the same
//                   ground truth
//   --jitter J      irregular game: presents every S input frames (--spacing,
//                   default the multiplier) times 1 +- J (uniform, --seed),
//                   each showing the frame of its own time — frame-time
//                   spikes of a variable-timestep game
//   --dup N         the game presents each image N times (e.g. 2: 30 fps
//                   content in a 60 fps loop); combines with --jitter
//   --refresh HZ    the display has a fixed refresh: frames show on its grid
//                   where the layer's pacing puts them, and each is scored
//                   against the content of that moment; --refresh-known also
//                   gives the engine the grid (as the present feedback does)
//   --hud-counter   draw a patch that changes on every present (64x16 at
//                   8,8: exclude it from the scores) — a HUD or cursor over a
//                   world that --dup repeats
//   --steady-content  with --jitter: a fixed-timestep game instead, image k
//                   shows time k*S and is presented within +-J*S/2 of it
//                   (only the present times jitter, the content does not)
//   --limit-shared KB, --limit-storage-images N   a smaller GPU's limits
//                   (Vulkan guarantees 16 KB and 4): the engine's fallbacks
//                   — a lean matcher only, or no generation — on any GPU
//   With --jitter, --dup or --spacing, the ground truth of every displayed
//   frame, real ones included, is the input frame at the time the player
//   should see then, following the layer's pacing (see the loop below), and
//   the manifest lists real frames too (the engine's output when it may
//   replace them, the input otherwise). Time runs as a 60 fps game.
// Settings come from BDEX_FG_* environment variables (the config file is
// ignored unless BDEX_FG_CONFIG names one), e.g. BDEX_FG_MODE=extrapolate.
//
// Output: <out_dir>/gen_<k>_<i>.ppm, the frame generated for input pair k at
// position i (1..multiplier-1), plus manifest.txt with one line per generated
// frame: "<generated file> <ground-truth file> <t> <previous input> <current input>".
// --verbose prints the engine's per-frame state (repeat flag, t of each slot).
// --score-real lists the real frames in the manifest too, as the engine
// outputs them (upscaled with --render-scale): scores the upscaler.
// --dump-flow DIR writes, per input pair k, DIR/flow_<k>.bin: the finest
// forward / backward flow and the level-0 lumas the flow was measured on
// (tools/train_flow.py) — header "BFLW", u32 w h block lw lh, f32 scale,
// then w*h*2 floats twice and lw*lh floats twice.
// --bench N repeats the flow N times per input, back to back (with
// temporal_prediction or accel the repetitions differ slightly).
// BDEX_FG_PROFILE=1 prints the mean GPU time per stage (analysis, flow,
// synthesis) of a fixed workload: a reproducible benchmark across GPUs,
// unlike a live game whose GPU clocks follow its load (still +-15 % from
// run to run: compare medians of a few runs).
#include "config.h"
#include "device.h"
#include "framegen.h"
#include "frame_order.h"
#include "log.h"

#include <algorithm>
#include <cmath>
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

uint16_t floatToHalf(float f) {  // round to nearest, finite inputs in [0, 1]
    if (f <= 0.f) return 0;
    int e;
    float m = std::frexp(f, &e);           // f = m * 2^e, m in [0.5, 1)
    int he = e - 1 + 15;                   // biased half exponent of 1.m form
    if (he <= 0) return uint16_t(std::lround(std::ldexp(f, 24)));  // subnormal
    uint32_t mant = uint32_t(std::lround((m * 2.f - 1.f) * 1024.f));
    if (mant == 1024) { mant = 0; ++he; }
    return uint16_t((he << 10) | mant);
}

float srgbToLinear(float v) {  // v in [0, 1]
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

float pqEncode(float nits) {  // SMPTE ST 2084 inverse EOTF
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    const float p = std::pow(std::max(nits, 0.f) / 10000.f, m1);
    return std::pow((c1 + c2 * p) / (1.f + c3 * p), m2);
}

// An SDR byte as the HDR "game" would output it, SDR white at 203 nits:
// scRGB (linear, 1 = 80 nits) or PQ, in [0, 1] code values for PQ.
float toHdr(float byte, VkColorSpaceKHR cs) {
    const float nits = srgbToLinear(byte / 255.f) * 203.f;
    return cs == VK_COLOR_SPACE_HDR10_ST2084_EXT ? pqEncode(nits) : nits / 80.f;
}

// The "game" frame in the engine's source format, at render size (bilinear
// downscale of the display-sized input when --render-scale < 1).
void encode(const Image& img, VkExtent2D render, VkFormat format, VkColorSpaceKHR cs, void* out) {
    const bool hdr = cs != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    const float sx = float(img.w) / render.width, sy = float(img.h) / render.height;
    for (uint32_t y = 0; y < render.height; ++y)
        for (uint32_t x = 0; x < render.width; ++x) {
            float c[3];
            if (render.width == img.w && render.height == img.h) {
                for (int k = 0; k < 3; ++k) c[k] = img.rgb[(size_t(y) * img.w + x) * 3 + k];
            } else {
                // Box filter over the source footprint of this pixel.
                const uint32_t x0 = uint32_t(x * sx), x1 = std::max(x0 + 1, uint32_t((x + 1) * sx));
                const uint32_t y0 = uint32_t(y * sy), y1 = std::max(y0 + 1, uint32_t((y + 1) * sy));
                float acc[3] = {0, 0, 0};
                for (uint32_t v = y0; v < y1 && v < img.h; ++v)
                    for (uint32_t u = x0; u < x1 && u < img.w; ++u)
                        for (int k = 0; k < 3; ++k) acc[k] += img.rgb[(size_t(v) * img.w + u) * 3 + k];
                const float n = float((std::min(x1, img.w) - x0) * (std::min(y1, img.h) - y0));
                for (int k = 0; k < 3; ++k) c[k] = acc[k] / n;
            }
            const size_t i = size_t(y) * render.width + x;
            switch (format) {
            case VK_FORMAT_B8G8R8A8_UNORM: {
                auto* d = static_cast<uint8_t*>(out) + i * 4;
                d[0] = uint8_t(std::lround(c[2])); d[1] = uint8_t(std::lround(c[1])); d[2] = uint8_t(std::lround(c[0])); d[3] = 255;
                break;
            }
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
                auto q = [&](float v) {
                    return uint32_t(std::lround(std::clamp(hdr ? toHdr(v, cs) : v / 255.f, 0.f, 1.f) * 1023.f));
                };
                static_cast<uint32_t*>(out)[i] = q(c[0]) | (q(c[1]) << 10) | (q(c[2]) << 20) | (3u << 30);
                break;
            }
            case VK_FORMAT_R16G16B16A16_SFLOAT: {
                auto* d = static_cast<uint16_t*>(out) + i * 4;
                for (int k = 0; k < 3; ++k) d[k] = floatToHalf(hdr ? toHdr(c[k], cs) : c[k] / 255.f);
                d[3] = floatToHalf(1.f);
                break;
            }
            default: {  // RGBA8 unorm / sRGB: the bytes as they are
                auto* d = static_cast<uint8_t*>(out) + i * 4;
                for (int k = 0; k < 3; ++k) d[k] = uint8_t(std::lround(c[k]));
                d[3] = 255;
            }
            }
        }
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
    // --list-options [--canonical]: every configuration key the engine
    // understands, aliases included unless --canonical (for
    // tests/test_option_keys.py), no GPU needed.
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--list-options") {
            const bool canonical = i + 1 < argc && std::string(argv[i + 1]) == "--canonical";
            for (const std::string& k : Config::optionKeys(!canonical)) printf("%s\n", k.c_str());
            return 0;
        }
    if (argc < 3) {
        fprintf(stderr, "usage: %s <in_dir> <out_dir> [--multiplier N] [--format F] [--colorspace C] [--render-scale S] [--gpu I] [--max-inputs N]\n", argv[0]);
        return 2;
    }
    const fs::path inDir = argv[1], outDir = argv[2];
    int mult = 2, gpu = -1, maxInputs = 0, dupCount = 1;
    double jitter = 0.0, spacing = 0.0;
    uint64_t seed = 1;
    bool verbose = false, steadyContent = false, scoreReal = false;
    int benchReps = 1;
    std::string flowDumpDir;
    bool hudCounter = false, refreshKnown = false;
    double refreshHz = 0.0;
    std::string fmtName = "rgba8", csName = "srgb";
    float renderScale = 1.f;
    // A smaller GPU's limits, as the engine sees them (its fallbacks are
    // tested on any GPU): KB of compute shared memory, storage images per stage.
    uint32_t limitShared = 0, limitStorage = 0;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--multiplier" && i + 1 < argc) mult = atoi(argv[++i]);
        else if (a == "--gpu" && i + 1 < argc) gpu = atoi(argv[++i]);
        else if (a == "--max-inputs" && i + 1 < argc) maxInputs = atoi(argv[++i]);
        else if (a == "--srgb") fmtName = "srgb";
        else if (a == "--format" && i + 1 < argc) fmtName = argv[++i];
        else if (a == "--render-scale" && i + 1 < argc) renderScale = float(atof(argv[++i]));
        else if (a == "--colorspace" && i + 1 < argc) csName = argv[++i];
        else if (a == "--jitter" && i + 1 < argc) jitter = atof(argv[++i]);
        else if (a == "--dup" && i + 1 < argc) dupCount = atoi(argv[++i]);
        else if (a == "--spacing" && i + 1 < argc) spacing = atof(argv[++i]);
        else if (a == "--seed" && i + 1 < argc) seed = strtoull(argv[++i], nullptr, 10);
        else if (a == "--verbose") verbose = true;
        else if (a == "--score-real") scoreReal = true;
        else if (a == "--steady-content") steadyContent = true;
        else if (a == "--bench" && i + 1 < argc) benchReps = std::max(1, atoi(argv[++i]));
        else if (a == "--dump-flow" && i + 1 < argc) flowDumpDir = argv[++i];
        else if (a == "--hud-counter") hudCounter = true;
        else if (a == "--refresh" && i + 1 < argc) refreshHz = atof(argv[++i]);
        else if (a == "--refresh-known") refreshKnown = true;
        else if (a == "--limit-shared" && i + 1 < argc) limitShared = uint32_t(atoi(argv[++i])) * 1024u;
        else if (a == "--limit-storage-images" && i + 1 < argc) limitStorage = uint32_t(atoi(argv[++i]));
        else { fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    if (mult < 2 || mult > 8) die("--multiplier must be 2 to 8");
    if (!(jitter >= 0.0 && jitter < 1.0)) die("--jitter must be in [0, 1)");
    if (dupCount < 1 || dupCount > 8) die("--dup must be 1..8");
    const bool schedule = jitter > 0.0 || dupCount > 1 || spacing > 0.0 || refreshHz > 0.0;
    if (spacing <= 0.0) spacing = mult;
    if (spacing < 1.0) die("--spacing must be >= 1");

    std::vector<fs::path> frames;
    for (auto& e : fs::directory_iterator(inDir))
        if (e.path().extension() == ".ppm") frames.push_back(e.path());
    std::sort(frames.begin(), frames.end());
    if (frames.size() < size_t(mult) + 1) die("not enough .ppm frames in the input directory");
    Image first;
    if (!readPpm(frames[0], first)) die("cannot read the first frame");
    const VkExtent2D extent{first.w, first.h};  // display size (the frames, and the ground truth)
    if (!(renderScale >= 0.25f && renderScale <= 1.f)) die("--render-scale must be in [0.25, 1]");
    const VkExtent2D render{std::max(1u, uint32_t(std::lround(first.w * renderScale))),
                            std::max(1u, uint32_t(std::lround(first.h * renderScale)))};
    VkFormat format;
    uint32_t bpp = 4;
    if (fmtName == "rgba8") format = VK_FORMAT_R8G8B8A8_UNORM;
    else if (fmtName == "bgra8") format = VK_FORMAT_B8G8R8A8_UNORM;
    else if (fmtName == "srgb") format = VK_FORMAT_R8G8B8A8_SRGB;
    else if (fmtName == "rgb10") format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    else if (fmtName == "rgba16f") { format = VK_FORMAT_R16G16B16A16_SFLOAT; bpp = 8; }
    else die("--format must be rgba8, bgra8, srgb, rgb10 or rgba16f");
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    if (csName == "scrgb") {
        if (format != VK_FORMAT_R16G16B16A16_SFLOAT) die("--colorspace scrgb needs --format rgba16f");
        colorSpace = VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
    } else if (csName == "hdr10") {
        if (format != VK_FORMAT_A2B10G10R10_UNORM_PACK32 && format != VK_FORMAT_R16G16B16A16_SFLOAT)
            die("--colorspace hdr10 needs --format rgb10 or rgba16f");
        colorSpace = VK_COLOR_SPACE_HDR10_ST2084_EXT;
    } else if (csName != "srgb") die("--colorspace must be srgb, scrgb or hdr10");

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
    if (limitShared) dev.props.limits.maxComputeSharedMemorySize = limitShared;
    if (limitStorage) dev.props.limits.maxPerStageDescriptorStorageImages = limitStorage;
    vkGetDeviceQueue(device, qf, 0, &dev.queue);
    dev.queueFamily = qf;
    dev.layerUsable = true;
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = qf;
    check(vkCreateCommandPool(device, &cp, nullptr, &dev.cmdPool), "vkCreateCommandPool");
    dev.config.autoConfigure(dev.props, dev.memProps);
    fprintf(stderr, "bdex_offline: %s, %s, render %ux%u -> %ux%u, x%d %s, %s\n", dev.props.deviceName, fmtName.c_str(),
            render.width, render.height, extent.width, extent.height,
            mult, dev.config.modeName(), dev.config.describe().c_str());

    int status = 0;
    {
        // Two history images play the role of the game's swapchain images.
        std::vector<AllocatedImage> hist(2);
        std::vector<VkImage> sources;
        for (auto& h : hist) {
            h = dev.createImage(format, render,
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT,
                                false);
            sources.push_back(h.image);
        }
        AllocatedBuffer staging = dev.createBuffer(VkDeviceSize(render.width) * render.height * bpp,
                                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        void* mapped = nullptr;
        check(vkMapMemory(device, staging.memory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
        auto fgp = std::make_unique<FrameGen>(dev, format, render, extent, static_cast<uint32_t>(mult - 1), sources,
                                              colorSpace);
        FrameGen& fg = *fgp;

        fs::create_directories(outDir);
        std::ofstream manifest(outDir / "manifest.txt");
        // Where the real frame goes among the frames of a present, and the
        // time of each generated one (frame_order.h, as the layer does).
        const FrameOrder order = FrameOrder::of(dev.config, mult);

        // The presents of the "game": which frame it shows (content) and when
        // (time, in input-frame units). By default input k is frame k*mult,
        // presented at a steady rate, and the frames in between are the
        // ground truth of the generated ones. --jitter / --dup instead model
        // an irregular game (see the usage at the top): presents at uneven
        // intervals, or the same image presented several times.
        struct Present { size_t content; double time; };
        std::vector<Present> sched;
        if (!schedule) {
            // Interpolation generates frames strictly between inputs k-1 and
            // k; extrapolation the frames after input k, so its last input
            // needs mult-1 ground-truth frames after it (hybrid: the frames
            // after the real one, m-1-real).
            const size_t ahead = size_t(mult - 1 - order.real);
            size_t nInputs = (frames.size() - 1 - ahead) / mult + 1;
            if (maxInputs > 0) nInputs = std::min(nInputs, size_t(maxInputs));
            for (size_t k = 0; k < nInputs; ++k) sched.push_back({k * mult, double(k * mult)});
        } else {
            uint64_t rng = seed * 0x9E3779B97F4A7C15ull + 1;
            auto uniform = [&] {  // [-1, 1), deterministic across platforms
                rng = rng * 6364136223846793005ull + 1442695040888963407ull;
                return double(rng >> 11) / double(1ull << 52) - 1.0;
            };
            double time = 0.0;
            const double last = double(frames.size() - 1) - 2.0 * spacing;  // room for the ground truth after
            for (size_t k = 0; time <= last && (maxInputs <= 0 || k < size_t(maxInputs)); ++k) {
                // With --dup N the image changes every N presents and shows
                // the moment of the first one (a 30 fps game in a 60 fps loop).
                const size_t first = k - k % size_t(dupCount);
                // --steady-content: a fixed-timestep game, whose image k shows
                // time k*S whenever it is actually presented.
                const double own = steadyContent ? double(k) * spacing : time;
                const double shown = first == k ? own : double(sched[first].content);
                sched.push_back({size_t(std::lround(shown)), time});
                if (steadyContent)  // presented late or early around its own slot, never drifting
                    time = std::max(time + 1.0, std::round(double(k + 1) * spacing + 0.5 * jitter * spacing * uniform()));
                else
                    time += std::max(1.0, std::round(spacing * (1.0 + jitter * uniform())));
            }
        }
        // Milliseconds per input frame: a steady present is a 60 fps game.
        const double msPerFrame = 1000.0 / 60.0 / double(schedule ? spacing : mult);

        int prev = -1;
        uint32_t parity = 0;
        double intervalMs = 0.0;  // the layer's smoothed present interval
        Image img;
        for (size_t k = 0; k < sched.size(); ++k) {
            const size_t fi = sched[k].content;
            if (!readPpm(frames[fi], img) || img.w != extent.width || img.h != extent.height) {
                fprintf(stderr, "bdex_offline: %s: unreadable or size differs from the first frame\n",
                        frames[fi].string().c_str());
                status = 1;
                break;
            }
            if (hudCounter) {
                // A HUD element redrawn on every present (a frame counter),
                // even when the world repeats: 64x16 pixels at (8, 8), to
                // leave out of the scores with --exclude 8,8,72,24.
                for (uint32_t y = 8; y < 24 && y < img.h; ++y)
                    for (uint32_t x = 8; x < 72 && x < img.w; ++x)
                        for (int c = 0; c < 3; ++c)
                            img.rgb[(size_t(y) * img.w + x) * 3 + c] =
                                uint8_t((k * 53 + ((x - 8) / 8) * 29 + c * 71) & 255);
            }
            encode(img, render, format, colorSpace, mapped);
            const int cur = static_cast<int>(k % 2);
            const bool generate = prev >= 0;
            FrameGen::FrameTiming timing;
            timing.multiplier = generate ? mult : 1;
            timing.reset = !generate;
            if (generate) {
                timing.sinceLastMs = (sched[k].time - sched[k - 1].time) * msPerFrame;
                intervalMs = intervalMs > 0.0 ? intervalMs * 0.8 + timing.sinceLastMs * 0.2 : timing.sinceLastMs;
            }
            timing.intervalMs = intervalMs;
            // --refresh: every frame of this present shows on a fixed refresh
            // grid (phase 0), where the layer's pacing puts it: frame k for
            // the refresh after present + I*k/m, frame 0 right after the GPU
            // work (1 ms). at[] is in input-frame units, relative to now.
            double at[8] = {};
            const int realIdx = order.real;
            if (refreshHz > 0.0 && generate) {
                const double pu = 1000.0 / refreshHz / msPerFrame, now = sched[k].time;
                const double Iu = intervalMs / msPerFrame;
                for (int j = 0; j < mult; ++j) {
                    const double want = now + (j == 0 ? 1.0 / msPerFrame : Iu * j / mult);
                    at[j] = std::ceil(want / pu) * pu - now;
                }
                if (refreshKnown) {  // what the layer passes once its present feedback knows the grid
                    timing.haveDisplay = true;
                    timing.displayMs[7] = float(at[realIdx] * msPerFrame);
                    for (int i = 1; i < mult; ++i)
                        timing.displayMs[i - 1] = float(at[order.position(i - 1)] * msPerFrame);
                }
            }
            const bool dumpReal = (schedule || scoreReal) && fg.packReal();
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
                region.imageExtent = {render.width, render.height, 1};
                vkCmdCopyBufferToImage(cmd, staging.buffer, hist[cur].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                       &region);
                b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                     nullptr, 0, nullptr, 1, &b);

                // The same sequence as VirtualSwapchain::present.
                fg.profileBegin(cmd, 0);
                fg.recordAcquireSources(cmd, prev, cur);
                fg.recordAnalysis(cmd, parity, static_cast<uint32_t>(cur));
                fg.recordFrameState(cmd, timing);
                fg.profileMark(cmd, 0, FrameGen::StageAnalysis);
                if (generate) {
                    // --bench: the same flow computed back to back keeps the GPU
                    // at full clock; the last repetition is the one used.
                    for (int r = 0; r < benchReps; ++r) fg.recordFlow(cmd, parity);
                    if (!flowDumpDir.empty()) fg.recordFlowDump(cmd, parity);
                    fg.profileMark(cmd, 0, FrameGen::StageFlow);
                    for (int i = 1; i < mult; ++i)
                        fg.recordInterpolate(cmd, static_cast<uint32_t>(prev), static_cast<uint32_t>(cur),
                                             order.t(i - 1), static_cast<uint32_t>(i - 1));
                    fg.profileMark(cmd, 0, FrameGen::StageInterp);
                    for (int i = 1; i < mult; ++i)
                        fg.recordDump(cmd, static_cast<uint32_t>(i - 1), static_cast<uint32_t>(cur));
                }
                if (fg.packReal()) fg.recordUpscaleReal(cmd, static_cast<uint32_t>(cur), generate ? prev : -1);
                if (dumpReal) fg.recordDump(cmd, fg.outputCount(), static_cast<uint32_t>(cur));
                fg.recordReleaseSources(cmd, prev, cur);
            });
            fg.profileCollect(0);
            if (generate && !flowDumpDir.empty()) {
                FrameGen::FlowDump d;
                if (fg.readFlowDump(d)) {
                    fs::create_directories(flowDumpDir);
                    char name[64];
                    snprintf(name, sizeof name, "flow_%04zu.bin", k);
                    std::ofstream f(fs::path(flowDumpDir) / name, std::ios::binary);
                    const uint32_t hdr[6] = {0x574C4642u /* BFLW */, d.w, d.h, d.block, d.lw, d.lh};
                    f.write(reinterpret_cast<const char*>(hdr), sizeof hdr);
                    f.write(reinterpret_cast<const char*>(&d.scale), 4);
                    f.write(reinterpret_cast<const char*>(d.fwd.data()), std::streamsize(d.fwd.size() * 4));
                    f.write(reinterpret_cast<const char*>(d.bwd.data()), std::streamsize(d.bwd.size() * 4));
                    f.write(reinterpret_cast<const char*>(d.lumaPrev.data()), std::streamsize(d.lumaPrev.size() * 4));
                    f.write(reinterpret_cast<const char*>(d.lumaCur.data()), std::streamsize(d.lumaCur.size() * 4));
                }
            }
            if (generate) {
                // What the player should see on each displayed frame: the
                // layer's pacing (swapchain.cpp) spreads the frames of a
                // present over the smoothed interval I. Interpolation shows
                // output i at +I(i-1)/m and the real frame at +I(m-1)/m, all
                // one latency L = I(m-1)/m late; extrapolation shows the real
                // frame at once and output i at +I*i/m. Steady presents give
                // exactly the frames between (or after) the inputs.
                const double I = intervalMs / msPerFrame, now = sched[k].time;
                auto truth = [&](double content, size_t& index) {
                    const double r = std::round(content);
                    if (r < 0.0 || r > double(frames.size() - 1)) return false;
                    index = size_t(r);
                    return true;
                };
                const std::string prevPath = fs::absolute(frames[sched[k - 1].content]).string();
                const std::string curPath = fs::absolute(frames[fi]).string();
                for (int i = 1; i < mult; ++i) {
                    size_t gt;
                    // With a refresh grid each frame shows the content of its
                    // display time; the real frame shows `now`.
                    const int pos = order.position(i - 1);
                    const double content = refreshHz > 0.0 ? now + at[pos] - at[realIdx]
                                           : schedule ? now + I * (pos - realIdx) / mult
                                                      : double(fi) + double(pos - realIdx);
                    if (!truth(content, gt)) continue;
                    char name[64];
                    snprintf(name, sizeof name, "gen_%04zu_%d.ppm", k, i);
                    if (!fg.writeDump((outDir / name).string(), static_cast<uint32_t>(i - 1))) {
                        fprintf(stderr, "bdex_offline: cannot write %s\n", name);
                        status = 1;
                    }
                    manifest << name << ' ' << fs::absolute(frames[gt]).string() << ' '
                             << order.t(i - 1) << ' ' << prevPath << ' ' << curPath << '\n';
                }
                size_t gt;
                if ((schedule || scoreReal) && truth(now, gt)) {
                    // The real frame (content time `now` in both display
                    // models); with dedup in extrapolation a repeat is
                    // replaced by an extrapolated frame, so score what the
                    // engine outputs.
                    std::string name = curPath;
                    if (dumpReal) {
                        char buf[64];
                        snprintf(buf, sizeof buf, "real_%04zu.ppm", k);
                        name = buf;
                        if (!fg.writeDump((outDir / name).string(), fg.outputCount())) status = 1;
                    }
                    manifest << name << ' ' << fs::absolute(frames[gt]).string() << " 1 "
                             << prevPath << ' ' << curPath << '\n';
                }
                FrameGen::State st;
                if (verbose && fg.readState(st))
                    fprintf(stderr, "present %zu: content %zu time %.1f dup %u t %.3f %.3f %.3f real %.3f\n", k, fi,
                            now, st.dup, st.t[0], st.t[1], st.t[2], st.t[7]);
            }
            prev = cur;
            parity ^= 1;
        }
        if (dev.config.profile)
            fprintf(stderr, "bdex_offline: gpu ms per input%s%s\n", fg.profileReport().c_str() + 9,
                    benchReps > 1 ? " (flow: all --bench repetitions)" : "");
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
