// Minimal Vulkan + GLFW test application: renders a procedural scene with
// moving objects at a capped frame rate so that the frame generation layer
// can be exercised without a game.
//
//   bdex_demo [--fps N] [--frames N] [--size WxH] [--mode fifo|mailbox|immediate]
//             [--deterministic] [--speed X] [--srgb] [--cut N] [--repeat N] [--duration S]
//             [--gpu-load N] [--latency-test [auto]] [--latency-log FILE]
//             [--stress KIND:N[,KIND:N...]] [--stress-until N]
//             [--replay DIR [--replay-step N] [--replay-start N]]
//
// --replay: a recorded sequence (fNNNN.ppm, e.g. from « Enregistrer » or
// tools/make_corpus.py) played as the game's frames — frame start + k*step
// on present k, scaled to the window — so the whole online path (layer,
// pacing, swapchain) runs on real content; tools/replay_bench.py scores the
// frames shown against the frames left out.
//
// Run it with BDEX_FG=1 (and VK_LAYER_PATH pointing at the build tree if the
// layer is not installed) to see the generated frames.
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "demo_shaders.h"

#define VK_CHECK(x)                                                                             \
    do {                                                                                        \
        VkResult _r = (x);                                                                      \
        if (_r < 0) throw std::runtime_error(std::string(#x) + " failed: " + std::to_string(_r)); \
    } while (0)

namespace {

struct Options {
    double fps = 30.0;
    long frames = -1;
    int width = 1280, height = 720;
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    bool deterministic = false;  // animation time advances by 1/fps per frame instead of wall clock
    bool ignoreResize = false;   // keep the swapchain through window resizes and VK_SUBOPTIMAL_KHR (like some games)
    bool srgb = false;           // request an sRGB swapchain format
    float speed = 1.0f;          // animation speed multiplier
    long cutEvery = 0;           // switch scene every N frames (0 = never), to test scene cut handling
    long repeat = 1;             // present every image N times (e.g. 2: 30 fps content in a 60 fps loop)
    double duration = 0.0;       // also stop after this many seconds (0 = no limit): bounds tests on slow drivers
    long gpuLoad = 1;            // draw the scene this many times a frame (same image): a GPU-bound game
    // --latency-test: an "input" (a key press, or with `auto` one every half
    // second, taken at the start of a frame) makes the next frame flat white;
    // --latency-log gets "flash <input ms> <present ms>" per flash (steady
    // clock, the layer's latency_log clock), for tools/latency_report.py.
    int latencyTest = 0;         // 0 off, 1 key presses, 2 automatic
    // --anti-lag: VK_AMD_anti_lag (Mesa's layer: ENABLE_LAYER_MESA_ANTI_LAG=1) as a
    // game uses it — its input stage at the start of each frame, before the
    // input is read, its present stage before presenting.
    bool antiLag = false;
    std::string latencyLog;
    // --stress: what games do to their swapchain, every N frames (tests/
    // swapchain_stress.sh): resize (the window to another size), recreate
    // (a new swapchain from the old one, same size, another present mode and
    // image count), surface (swapchain and surface destroyed, new ones on the
    // same window), window (a new window altogether), fullscreen (toggled on
    // the primary monitor), minimize (iconified for a few frames). Until
    // frame --stress-until, steady afterwards.
    long stressEvery[6] = {};
    long stressUntil = -1;
    std::string replayDir;  // --replay: the frames (sorted .ppm) shown instead of the scene
    long replayStep = 1;    // --replay-step: recorded frames per present (the ground truth in between)
    long replayStart = 0;   // --replay-start
};
enum Stress { kResize, kRecreate, kSurface, kWindow, kFullscreen, kMinimize, kStressKinds };
const char* const kStressNames[kStressKinds] = {"resize", "recreate", "surface", "window", "fullscreen", "minimize"};

struct PushConstants {
    float resolution[2];
    float time;
    uint32_t frame;
    uint32_t variant;
};

class Demo {
public:
    explicit Demo(const Options& o) : opt_(o) {}
    ~Demo() { cleanup(); }
    void run();

private:
    void initWindow();
    void initVulkan();
    void createSwapchain();
    void destroySwapchain();
    void createPipeline();
    void drawFrame();
    void cleanup();
    void makeWindow();
    void stress();
    void recreateSwapchain();
    void initReplay();
    void recordReplay(VkCommandBuffer cmd, uint32_t imageIndex, int fi);
    uint32_t findMemory(uint32_t bits, VkMemoryPropertyFlags flags);

    Options opt_;
    GLFWwindow* window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physDev_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkFramebuffer> framebuffers_;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;

    static constexpr int kInFlight = 2;
    VkCommandBuffer cmds_[kInFlight]{};
    VkSemaphore acquireSem_[kInFlight]{};
    std::vector<VkSemaphore> renderSem_;  // per swapchain image
    VkFence fences_[kInFlight]{};
    int frameIndex_ = 0;
    double animTime_ = 0.0;  // animation time of the image being drawn
    uint32_t frameCount_ = 0;
    bool resized_ = false;
    double inputMs_ = -1.0;    // --latency-test: a pending input (steady ms), -1 none
    uint64_t antiLagFrame_ = 0;
#ifdef VK_AMD_anti_lag  // (headers from 1.3.296 on)
    PFN_vkAntiLagUpdateAMD antiLagUpdate_ = nullptr;  // --anti-lag
    void antiLag(bool input) {
        if (!antiLagUpdate_) return;
        VkAntiLagPresentationInfoAMD pi{VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD};
        pi.stage = input ? VK_ANTI_LAG_STAGE_INPUT_AMD : VK_ANTI_LAG_STAGE_PRESENT_AMD;
        pi.frameIndex = antiLagFrame_;
        VkAntiLagDataAMD d{VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD};
        d.mode = VK_ANTI_LAG_MODE_ON_AMD;
        d.pPresentationInfo = &pi;
        antiLagUpdate_(device_, &d);
    }
#else
    void antiLag(bool) {}
#endif
    double nextAutoMs_ = 0.0;
    FILE* latencyLog_ = nullptr;
    // --stress state: present mode and image count overrides (recreate), a
    // pseudo-random sequence, frames left iconified, fullscreen on.
    int stressMode_ = -1, stressCount_ = 0;
    uint32_t stressRng_ = 12345u;
    int minimizedFrames_ = 0;
    bool fullscreen_ = false;
    uint32_t lastStress_ = 0;
    // --replay: the frames, a staging buffer per frame in flight and the
    // image they are copied into, then blitted (scaled) to the swapchain's.
    std::vector<std::filesystem::path> replayFrames_;
    uint32_t replayW_ = 0, replayH_ = 0;
    VkBuffer replayBuf_[kInFlight]{};
    VkDeviceMemory replayBufMem_[kInFlight]{};
    void* replayMapped_[kInFlight]{};
    VkImage replayImage_[kInFlight]{};
    VkDeviceMemory replayImageMem_[kInFlight]{};
};

// A binary PPM (P6, 8 bits): its size, and its pixels as RGBA when `rgba`.
bool readPpm(const std::filesystem::path& p, uint32_t& w, uint32_t& h, uint8_t* rgba) {
    std::ifstream in(p, std::ios::binary);
    std::string magic;
    int maxv = 0;
    in >> magic >> w >> h >> maxv;
    in.get();
    if (!in || magic != "P6" || maxv != 255) return false;
    if (!rgba) return true;
    std::vector<uint8_t> rgb(size_t(w) * h * 3);
    if (!in.read(reinterpret_cast<char*>(rgb.data()), std::streamsize(rgb.size()))) return false;
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        rgba[i * 4] = rgb[i * 3];
        rgba[i * 4 + 1] = rgb[i * 3 + 1];
        rgba[i * 4 + 2] = rgb[i * 3 + 2];
        rgba[i * 4 + 3] = 255;
    }
    return true;
}

double steadyMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Demo::initWindow() {
    if (!glfwInit()) throw std::runtime_error("glfwInit failed");
    if (!opt_.latencyLog.empty()) {
        latencyLog_ = fopen(opt_.latencyLog.c_str(), "w");
        if (!latencyLog_) throw std::runtime_error("cannot write " + opt_.latencyLog);
    }
    makeWindow();
}

void Demo::makeWindow() {
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    // Deterministic runs (the quality eval) must start at the same window
    // size every time; a tiling window manager otherwise re-tiles the window
    // (e.g. when several demos run concurrently) and the two eval sequences
    // land at different sizes. A fixed-size window cannot be resized.
    glfwWindowHint(GLFW_RESIZABLE, opt_.deterministic ? GLFW_FALSE : GLFW_TRUE);
    window_ = glfwCreateWindow(opt_.width, opt_.height, "bdex frame generation demo", nullptr, nullptr);
    if (!window_) throw std::runtime_error("glfwCreateWindow failed");
    glfwSetWindowUserPointer(window_, this);
    if (opt_.latencyTest == 1)
        glfwSetKeyCallback(window_, [](GLFWwindow* w, int key, int, int action, int) {
            auto* d = static_cast<Demo*>(glfwGetWindowUserPointer(w));
            if (action == GLFW_PRESS && key != GLFW_KEY_ESCAPE && d->inputMs_ < 0.0) d->inputMs_ = steadyMs();
        });
    glfwSetFramebufferSizeCallback(window_, [](GLFWwindow* w, int, int) {
        static_cast<Demo*>(glfwGetWindowUserPointer(w))->resized_ = true;
    });
    // The window manager maps the window at a transient size and resizes it
    // afterwards; the first swapchain must not be created on that transient
    // size (it breaks the deterministic eval alignment). Wait until the
    // framebuffer size has been stable for a few polls.
    int w = 0, h = 0, lw = -1, lh = -1, stable = 0;
    for (int i = 0; i < 100 && stable < 3; ++i) {
        glfwWaitEventsTimeout(0.05);
        glfwGetFramebufferSize(window_, &w, &h);
        stable = (w > 0 && w == lw && h == lh) ? stable + 1 : 0;
        lw = w; lh = h;
    }
}

void Demo::initVulkan() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "bdex_demo";
    app.apiVersion = VK_API_VERSION_1_1;
    uint32_t extCount = 0;
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&extCount);
    std::vector<const char*> exts(glfwExts, glfwExts + extCount);
    // As DXVK and VKD3D-Proton do (the layer's present timing reads the
    // surface's capabilities through it): when the loader has it.
    uint32_t nAvail = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &nAvail, nullptr);
    std::vector<VkExtensionProperties> avail(nAvail);
    vkEnumerateInstanceExtensionProperties(nullptr, &nAvail, avail.data());
    for (const auto& e : avail)
        if (strcmp(e.extensionName, "VK_KHR_get_surface_capabilities2") == 0) exts.push_back("VK_KHR_get_surface_capabilities2");
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    VK_CHECK(vkCreateInstance(&ici, nullptr, &instance_));
    VK_CHECK(glfwCreateWindowSurface(instance_, window_, nullptr, &surface_));

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(instance_, &n, devs.data());
    for (VkPhysicalDevice d : devs) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qf.data());
        for (uint32_t i = 0; i < qn; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &present);
            if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                physDev_ = d;
                queueFamily_ = i;
                break;
            }
        }
        if (physDev_) break;
    }
    if (!physDev_) throw std::runtime_error("no suitable GPU");
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physDev_, &props);
    printf("demo: using %s\n", props.deviceName);

    float prio = 1.f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
#ifdef VK_AMD_anti_lag
    const char* devExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_AMD_ANTI_LAG_EXTENSION_NAME};
#else
    const char* devExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
#endif
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = devExts;
#ifdef VK_AMD_anti_lag
    VkPhysicalDeviceAntiLagFeaturesAMD antiLagFeature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD};
    if (opt_.antiLag) {
        uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(physDev_, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> exts(n);
        vkEnumerateDeviceExtensionProperties(physDev_, nullptr, &n, exts.data());
        bool found = false;
        for (const auto& e : exts) found = found || strcmp(e.extensionName, VK_AMD_ANTI_LAG_EXTENSION_NAME) == 0;
        if (!found) throw std::runtime_error("--anti-lag: VK_AMD_anti_lag not available (ENABLE_LAYER_MESA_ANTI_LAG=1?)");
        antiLagFeature.antiLag = VK_TRUE;
        dci.pNext = &antiLagFeature;
        dci.enabledExtensionCount = 2;
    }
#else
    if (opt_.antiLag) throw std::runtime_error("--anti-lag: built with Vulkan headers older than VK_AMD_anti_lag");
#endif
    VK_CHECK(vkCreateDevice(physDev_, &dci, nullptr, &device_));
#ifdef VK_AMD_anti_lag
    if (opt_.antiLag)
        antiLagUpdate_ = reinterpret_cast<PFN_vkAntiLagUpdateAMD>(vkGetDeviceProcAddr(device_, "vkAntiLagUpdateAMD"));
#endif
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily_;
    VK_CHECK(vkCreateCommandPool(device_, &pci, nullptr, &pool_));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = kInFlight;
    VK_CHECK(vkAllocateCommandBuffers(device_, &cai, cmds_));
    for (int i = 0; i < kInFlight; ++i) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(device_, &si, nullptr, &acquireSem_[i]));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(device_, &fi, nullptr, &fences_[i]));
    }

    createSwapchain();
    createPipeline();
    if (!opt_.replayDir.empty()) initReplay();
}

void Demo::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physDev_, surface_, &caps));
    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physDev_, surface_, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fn);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physDev_, surface_, &fn, formats.data());
    VkSurfaceFormatKHR chosen = formats[0];
    const VkFormat wanted = opt_.srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
    for (auto& f : formats)
        if (f.format == wanted && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) chosen = f;
    format_ = chosen.format;

    uint32_t mn = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physDev_, surface_, &mn, nullptr);
    std::vector<VkPresentModeKHR> modes(mn);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physDev_, surface_, &mn, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (std::find(modes.begin(), modes.end(), opt_.mode) != modes.end()) mode = opt_.mode;
    if (stressMode_ >= 0) {  // a core mode the surface supports (others need features enabled)
        std::vector<VkPresentModeKHR> core;
        for (VkPresentModeKHR m : modes)
            if (m <= VK_PRESENT_MODE_FIFO_RELAXED_KHR) core.push_back(m);
        if (!core.empty()) mode = core[stressMode_ % core.size()];
    }

    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    if (caps.currentExtent.width != UINT32_MAX) extent_ = caps.currentExtent;
    else extent_ = {std::clamp<uint32_t>(w, caps.minImageExtent.width, caps.maxImageExtent.width),
                    std::clamp<uint32_t>(h, caps.minImageExtent.height, caps.maxImageExtent.height)};

    uint32_t count = std::max(caps.minImageCount, 3u);
    if (stressMode_ >= 0) count = caps.minImageCount + stressCount_;  // min, min + 1, min + 2
    if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface_;
    sci.minImageCount = count;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent_;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (opt_.replayDir.empty() ? VkImageUsageFlags(0) : VkImageUsageFlags(VK_IMAGE_USAGE_TRANSFER_DST_BIT));
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = swapchain_;
    VkSwapchainKHR old = swapchain_;
    VK_CHECK(vkCreateSwapchainKHR(device_, &sci, nullptr, &swapchain_));
    if (old) vkDestroySwapchainKHR(device_, old, nullptr);

    uint32_t in = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &in, nullptr);
    images_.resize(in);
    vkGetSwapchainImagesKHR(device_, swapchain_, &in, images_.data());
    printf("demo: swapchain %ux%u, %u images, present mode %d\n", extent_.width, extent_.height, in, mode);

    if (!renderPass_) {
        VkAttachmentDescription att{};
        att.format = format_;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = 0;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rci.attachmentCount = 1;
        rci.pAttachments = &att;
        rci.subpassCount = 1;
        rci.pSubpasses = &sub;
        rci.dependencyCount = 1;
        rci.pDependencies = &dep;
        VK_CHECK(vkCreateRenderPass(device_, &rci, nullptr, &renderPass_));
    }

    views_.resize(in);
    framebuffers_.resize(in);
    for (uint32_t i = 0; i < in; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = images_[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format_;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &views_[i]));
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = renderPass_;
        fci.attachmentCount = 1;
        fci.pAttachments = &views_[i];
        fci.width = extent_.width;
        fci.height = extent_.height;
        fci.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device_, &fci, nullptr, &framebuffers_[i]));
    }
    for (VkSemaphore s : renderSem_) vkDestroySemaphore(device_, s, nullptr);
    renderSem_.assign(in, VK_NULL_HANDLE);
    for (auto& s : renderSem_) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(device_, &si, nullptr, &s));
    }
}

void Demo::destroySwapchain() {
    for (VkFramebuffer f : framebuffers_) vkDestroyFramebuffer(device_, f, nullptr);
    for (VkImageView v : views_) vkDestroyImageView(device_, v, nullptr);
    framebuffers_.clear();
    views_.clear();
}

void Demo::createPipeline() {
    auto makeModule = [&](const uint32_t* code, size_t size) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = size;
        ci.pCode = code;
        VkShaderModule m;
        VK_CHECK(vkCreateShaderModule(device_, &ci, nullptr, &m));
        return m;
    };
    VkShaderModule vs = makeModule(fullscreen_vert, fullscreen_vert_size);
    VkShaderModule fs = makeModule(scene_frag, scene_frag_size);

    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &pcr;
    VK_CHECK(vkCreatePipelineLayout(device_, &lci, nullptr, &layout_));

    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &ds;
    pci.layout = layout_;
    pci.renderPass = renderPass_;
    VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline_));
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
}

void Demo::drawFrame() {
    static const auto start = std::chrono::steady_clock::now();
    const int fi = frameIndex_;
    ++antiLagFrame_;
    antiLag(true);  // (--anti-lag: its input stage, before anything of the frame, input included)
    vkWaitForFences(device_, 1, &fences_[fi], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, acquireSem_[fi], VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        vkDeviceWaitIdle(device_);
        destroySwapchain();
        createSwapchain();
        return;
    }
    if (r < 0) throw std::runtime_error("vkAcquireNextImageKHR failed");
    vkResetFences(device_, 1, &fences_[fi]);

    VkCommandBuffer cmd = cmds_[fi];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    double flashInput = -1.0;
    if (!opt_.replayDir.empty()) {
        recordReplay(cmd, imageIndex, fi);
        goto submit;
    }
    {
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = renderPass_;
    rbi.framebuffer = framebuffers_[imageIndex];
    rbi.renderArea = {{0, 0}, extent_};
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{0.f, 0.f, float(extent_.width), float(extent_.height), 0.f, 1.f};
    VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    // --repeat: the frame shown is the first of its group of N, identical
    // to the others (counter included), like a game duplicating frames.
    const auto shown = frameCount_ - frameCount_ % static_cast<decltype(frameCount_)>(opt_.repeat);
    if (opt_.deterministic) animTime_ = shown / opt_.fps;
    else if (shown == frameCount_)  // the group keeps its first frame's wall-clock time
        animTime_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    PushConstants pc{{float(extent_.width), float(extent_.height)}, float(animTime_ * opt_.speed), shown,
                     opt_.cutEvery > 0 ? uint32_t((shown / opt_.cutEvery) & 1) : 0u};
    // --latency-test: this frame is the answer to the pending input — white.
    if (opt_.latencyTest == 2 && steadyMs() >= nextAutoMs_) {
        if (nextAutoMs_ > 0.0) inputMs_ = steadyMs();  // (none in the first half second)
        nextAutoMs_ = steadyMs() + 500.0;
    }
    if (opt_.latencyTest && inputMs_ >= 0.0) {
        flashInput = inputMs_;
        inputMs_ = -1.0;
        pc.variant = 2u;
    }
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    for (long i = 0; i < opt_.gpuLoad; ++i) vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    }
submit:
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags wait = opt_.replayDir.empty() ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquireSem_[fi];
    si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &renderSem_[imageIndex];
    VK_CHECK(vkQueueSubmit(queue_, 1, &si, fences_[fi]));

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderSem_[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex;
    if (flashInput >= 0.0 && latencyLog_) {
        fprintf(latencyLog_, "flash %.3f %.3f\n", flashInput, steadyMs());
        fflush(latencyLog_);
    }
    antiLag(false);
    r = vkQueuePresentKHR(queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || (!opt_.ignoreResize && (r == VK_SUBOPTIMAL_KHR || resized_))) {
        resized_ = false;
        vkDeviceWaitIdle(device_);
        destroySwapchain();
        createSwapchain();
    } else if (r < 0) {
        throw std::runtime_error("vkQueuePresentKHR failed");
    }
    frameIndex_ = (frameIndex_ + 1) % kInFlight;
    ++frameCount_;
}

uint32_t Demo::findMemory(uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(physDev_, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags) return i;
    throw std::runtime_error("no suitable memory type");
}

void Demo::initReplay() {
    for (const auto& e : std::filesystem::directory_iterator(opt_.replayDir))
        if (e.path().extension() == ".ppm") replayFrames_.push_back(e.path());
    std::sort(replayFrames_.begin(), replayFrames_.end());
    if (replayFrames_.empty() || !readPpm(replayFrames_[0], replayW_, replayH_, nullptr))
        throw std::runtime_error("--replay: no readable .ppm frame in " + opt_.replayDir);
    printf("demo: replay of %zu frames %ux%u (every %ld from %ld)\n", replayFrames_.size(), replayW_, replayH_,
           opt_.replayStep, opt_.replayStart);
    const VkDeviceSize bytes = VkDeviceSize(replayW_) * replayH_ * 4;
    for (int i = 0; i < kInFlight; ++i) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VK_CHECK(vkCreateBuffer(device_, &bci, nullptr, &replayBuf_[i]));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, replayBuf_[i], &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = findMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &replayBufMem_[i]));
        VK_CHECK(vkBindBufferMemory(device_, replayBuf_[i], replayBufMem_[i], 0));
        VK_CHECK(vkMapMemory(device_, replayBufMem_[i], 0, VK_WHOLE_SIZE, 0, &replayMapped_[i]));
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {replayW_, replayH_, 1};
        ici.mipLevels = ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VK_CHECK(vkCreateImage(device_, &ici, nullptr, &replayImage_[i]));
        vkGetImageMemoryRequirements(device_, replayImage_[i], &req);
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = findMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &replayImageMem_[i]));
        VK_CHECK(vkBindImageMemory(device_, replayImage_[i], replayImageMem_[i], 0));
    }
}

// Present k shows recorded frame start + k*step (the last one held past the
// end): loaded into this frame's staging buffer, copied, blitted to the
// swapchain image at its size.
void Demo::recordReplay(VkCommandBuffer cmd, uint32_t imageIndex, int fi) {
    const size_t n = replayFrames_.size();
    const size_t idx = std::min(n - 1, size_t(opt_.replayStart) + size_t(frameCount_) * size_t(opt_.replayStep));
    uint32_t w = 0, h = 0;
    if (!readPpm(replayFrames_[idx], w, h, static_cast<uint8_t*>(replayMapped_[fi])) || w != replayW_ || h != replayH_)
        throw std::runtime_error("--replay: " + replayFrames_[idx].string() + " unreadable or of another size");
    auto barrier = [&](VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from;
        b.newLayout = to;
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    barrier(replayImage_[fi], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {replayW_, replayH_, 1};
    vkCmdCopyBufferToImage(cmd, replayBuf_[fi], replayImage_[fi], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(replayImage_[fi], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    barrier(images_[imageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit blit{};
    blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(replayW_), int32_t(replayH_), 1};
    blit.dstOffsets[1] = {int32_t(extent_.width), int32_t(extent_.height), 1};
    vkCmdBlitImage(cmd, replayImage_[fi], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images_[imageIndex],
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    barrier(images_[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0);
}

void Demo::recreateSwapchain() {
    vkDeviceWaitIdle(device_);
    destroySwapchain();
    createSwapchain();
}

void Demo::stress() {
    if (opt_.stressUntil >= 0 && frameCount_ >= static_cast<uint32_t>(opt_.stressUntil)) return;
    if (frameCount_ == 0 || frameCount_ == lastStress_) return;  // (once a frame: minimized, out of date)
    lastStress_ = frameCount_;
    auto due = [&](Stress k) { return opt_.stressEvery[k] > 0 && frameCount_ % opt_.stressEvery[k] == 0; };
    auto rnd = [&](uint32_t n) {
        stressRng_ = stressRng_ * 1664525u + 1013904223u;
        return (stressRng_ >> 8) % n;
    };
    if (due(kResize)) {
        const int w = 160 + int(rnd(uint32_t(std::max(1, opt_.width - 159))));
        const int h = 120 + int(rnd(uint32_t(std::max(1, opt_.height - 119))));
        printf("demo: stress resize %dx%d\n", w, h);
        glfwSetWindowSize(window_, w, h);
    }
    if (due(kRecreate)) {
        stressMode_ = int(rnd(8));
        stressCount_ = int(rnd(3));
        printf("demo: stress recreate\n");
        recreateSwapchain();
    }
    if (due(kSurface) || due(kWindow)) {
        const bool window = due(kWindow);
        printf("demo: stress %s\n", window ? "window" : "surface");
        vkDeviceWaitIdle(device_);
        destroySwapchain();
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
        if (window) {
            glfwDestroyWindow(window_);
            fullscreen_ = false;
            makeWindow();
        }
        VK_CHECK(glfwCreateWindowSurface(instance_, window_, nullptr, &surface_));
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physDev_, queueFamily_, surface_, &present);
        if (!present) throw std::runtime_error("the new surface cannot be presented to");
        createSwapchain();
    }
    if (due(kFullscreen)) {
        GLFWmonitor* mon = glfwGetPrimaryMonitor();
        const GLFWvidmode* vm = mon ? glfwGetVideoMode(mon) : nullptr;
        fullscreen_ = !fullscreen_ && vm;
        printf("demo: stress fullscreen %s\n", fullscreen_ ? "on" : "off");
        if (fullscreen_) glfwSetWindowMonitor(window_, mon, 0, 0, vm->width, vm->height, vm->refreshRate);
        else glfwSetWindowMonitor(window_, nullptr, 40, 40, opt_.width, opt_.height, 0);
    }
    if (due(kMinimize)) {
        printf("demo: stress minimize\n");
        glfwIconifyWindow(window_);
        minimizedFrames_ = 5;
    }
}

void Demo::run() {
    initWindow();
    initVulkan();
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / opt_.fps));
    auto next = clock::now();
    auto statT = clock::now();
    const auto runStart = statT;
    uint32_t statFrames = 0;
    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        if (glfwGetKey(window_, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
        stress();
        if (minimizedFrames_ > 0 && --minimizedFrames_ == 0) glfwRestoreWindow(window_);
        int w = 0, h = 0;
        glfwGetFramebufferSize(window_, &w, &h);
        if (w == 0 || h == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (minimizedFrames_ == 0 && opt_.stressEvery[kMinimize] > 0) glfwRestoreWindow(window_);
            continue;
        }
        drawFrame();
        ++statFrames;
        if (opt_.frames >= 0 && frameCount_ >= static_cast<uint32_t>(opt_.frames)) break;
        if (opt_.duration > 0.0 && std::chrono::duration<double>(clock::now() - runStart).count() >= opt_.duration) break;
        const auto now = clock::now();
        if (std::chrono::duration<double>(now - statT).count() >= 2.0) {
            char title[128];
            snprintf(title, sizeof(title), "bdex demo - %.1f fps rendered (target %.0f)",
                     statFrames / std::chrono::duration<double>(now - statT).count(), opt_.fps);
            glfwSetWindowTitle(window_, title);
            printf("%s\n", title);
            statT = now;
            statFrames = 0;
        }
        next += period;
        if (next > clock::now()) std::this_thread::sleep_until(next);
        else next = clock::now();
    }
    vkDeviceWaitIdle(device_);
}

void Demo::cleanup() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        destroySwapchain();
        for (VkSemaphore s : renderSem_) vkDestroySemaphore(device_, s, nullptr);
        for (int i = 0; i < kInFlight; ++i) {
            if (acquireSem_[i]) vkDestroySemaphore(device_, acquireSem_[i], nullptr);
            if (fences_[i]) vkDestroyFence(device_, fences_[i], nullptr);
        }
        if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        for (int i = 0; i < kInFlight; ++i) {
            if (replayBuf_[i]) vkDestroyBuffer(device_, replayBuf_[i], nullptr);
            if (replayBufMem_[i]) vkFreeMemory(device_, replayBufMem_[i], nullptr);
            if (replayImage_[i]) vkDestroyImage(device_, replayImage_[i], nullptr);
            if (replayImageMem_[i]) vkFreeMemory(device_, replayImageMem_[i], nullptr);
        }
        if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
        if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
        if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);
        if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
    if (latencyLog_) fclose(latencyLog_);
    if (window_) glfwDestroyWindow(window_);
    glfwTerminate();
}

Options parseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
            return argv[++i];
        };
        if (a == "--fps") o.fps = std::max(1.0, atof(value().c_str()));
        else if (a == "--frames") o.frames = atol(value().c_str());
        else if (a == "--deterministic") o.deterministic = true;
        else if (a == "--ignore-resize") o.ignoreResize = true;
        else if (a == "--srgb") o.srgb = true;
        else if (a == "--cut") o.cutEvery = atol(value().c_str());
        else if (a == "--speed") o.speed = atof(value().c_str());
        else if (a == "--repeat") o.repeat = std::max(1L, atol(value().c_str()));
        else if (a == "--gpu-load") o.gpuLoad = std::clamp(atol(value().c_str()), 1L, 1000L);
        else if (a == "--duration") o.duration = std::max(0.0, atof(value().c_str()));
        else if (a == "--latency-test") {
            o.latencyTest = 1;
            if (i + 1 < argc && std::string(argv[i + 1]) == "auto") { o.latencyTest = 2; ++i; }
        } else if (a == "--latency-log") o.latencyLog = value();
        else if (a == "--anti-lag") o.antiLag = true;
        else if (a == "--size") {
            std::string v = value();
            if (sscanf(v.c_str(), "%dx%d", &o.width, &o.height) != 2) throw std::runtime_error("bad --size");
        } else if (a == "--mode") {
            std::string v = value();
            if (v == "fifo") o.mode = VK_PRESENT_MODE_FIFO_KHR;
            else if (v == "mailbox") o.mode = VK_PRESENT_MODE_MAILBOX_KHR;
            else if (v == "immediate") o.mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            else throw std::runtime_error("bad --mode");
        } else if (a == "--stress") {
            std::string v = value();
            for (size_t p = 0; p < v.size();) {
                const size_t e = std::min(v.find(',', p), v.size()), c = v.find(':', p);
                if (c == std::string::npos || c > e) throw std::runtime_error("bad --stress (KIND:N)");
                const std::string kind = v.substr(p, c - p);
                int k = 0;
                while (k < kStressKinds && kind != kStressNames[k]) ++k;
                if (k == kStressKinds) throw std::runtime_error("bad --stress kind " + kind);
                o.stressEvery[k] = std::max(0L, atol(v.substr(c + 1, e - c - 1).c_str()));
                p = e + 1;
            }
        } else if (a == "--stress-until") o.stressUntil = atol(value().c_str());
        else if (a == "--replay") o.replayDir = value();
        else if (a == "--replay-step") o.replayStep = std::max(1L, atol(value().c_str()));
        else if (a == "--replay-start") o.replayStart = std::max(0L, atol(value().c_str()));
        else if (a == "-h" || a == "--help") {
            printf("usage: bdex_demo [--fps N] [--frames N] [--size WxH] [--mode fifo|mailbox|immediate]\n"
                   "                 [--deterministic] [--ignore-resize] [--speed X] [--srgb] [--cut N] [--repeat N] [--duration S]\n"
                   "                 [--gpu-load N] [--latency-test [auto]] [--latency-log FILE] [--anti-lag]\n"
                   "                 [--stress resize|recreate|surface|window|fullscreen|minimize:N[,...]] [--stress-until N]\n"
                   "                 [--replay DIR [--replay-step N] [--replay-start N]]\n");
            exit(0);
        } else {
            throw std::runtime_error("unknown option " + a);
        }
    }
    return o;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);  // in order with the layer's log (stderr) when piped
    try {
        Demo demo(parseArgs(argc, argv));
        demo.run();
    } catch (const std::exception& e) {
        fprintf(stderr, "demo: %s\n", e.what());
        return 1;
    }
    return 0;
}
