// Minimal Vulkan + GLFW test application: renders a procedural scene with
// moving objects at a capped frame rate so that the frame generation layer
// can be exercised without a game.
//
//   bdex_demo [--fps N] [--frames N] [--size WxH] [--mode fifo|mailbox|immediate]
//             [--deterministic] [--speed X] [--srgb] [--cut N] [--repeat N] [--duration S]
//             [--gpu-load N] [--latency-test [auto]] [--latency-log FILE]
//             [--present-thread L [--engine NAME] [--sim-thread [--reflex [--reflex-limit]]]]
//             [--stress KIND:N[,KIND:N...]] [--stress-until N]
//             [--replay DIR [--replay-step N] [--replay-start N]
//              [--replay-timing [--replay-log FILE]]]
//
// --replay: a recorded sequence (fNNNN.ppm, e.g. from « Enregistrer » or
// tools/make_corpus.py) played as the game's frames — frame start + k*step
// on present k, scaled to the window — so the whole online path (layer,
// pacing, swapchain) runs on real content; tools/replay_bench.py scores the
// frames shown against the frames left out.
// --present-thread: presents as DXVK and VKD3D-Proton do — see Demo::ptQueueLoop.
// --replay-timing uses the recording's timestamps.csv instead of --fps and
// stops after the last selected source. --replay-log records source offsets,
// requested deadlines, actual presents and overload shifts (steady ms).
//
// Run it with BDEX_FG=1 (and VK_LAYER_PATH pointing at the build tree if the
// layer is not installed) to see the generated frames.
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "demo_shaders.h"
#include "replay_timing.h"

#define VK_CHECK(x)                                                                             \
    do {                                                                                        \
        VkResult _r = (x);                                                                      \
        if (_r < 0) throw std::runtime_error(std::string(#x) + " failed: " + std::to_string(_r)); \
    } while (0)

namespace {

struct Options {
    double fps = 30.0;
    bool uncapped = false;      // run as fast as rendering allows (cadence stress tests)
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
    // --present-thread L: as a Direct3D game under Proton presents (DXVK,
    // VKD3D-Proton): rendered here, submitted and presented by a present
    // thread, the next frame begun once present k - L + 1 is done (DXGI's
    // frame latency); --engine: the engine name the layer sees ("vkd3d" by
    // default with --present-thread: fps_limit_wait offers it present wait).
    int presentThread = 0;
    std::string engine;
    // --sim-thread (with --present-thread): a game thread of its own reads
    // the input and simulates, at most one frame ahead of the thread that
    // renders and presents — Unreal's game and render threads (FFrameEndSync
    // with one frame of lag), RE Engine's. Its own cap (--fps) sleeps there,
    // before the input, as those engines' limiters do; --reflex: so does
    // NVIDIA Reflex's sleep (VK_NV_low_latency2's vkLatencySleepNV, with its
    // markers), which a game turns on in its settings; --reflex-limit: the
    // cap (--fps) is Reflex's own (minimumIntervalUs, the driver's limiter),
    // as Unreal hands it its frame rate limit with Reflex on.
    bool simThread = false;
    bool reflex = false;
    bool reflexLimit = false;
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
    bool replayTiming = false;  // --replay-timing: timestamps.csv instead of --fps, stop at the last source
    std::string replayLog;      // --replay-log: requested and actual source present times
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
    void drawFramePT();
    void ptQueueLoop();
    void ptWaitLoop();
    void ptCreateTargets();
    void ptDestroyTargets();
    void ptDrain();
    void ptStopThreads();
    void simLoop();
    void reflexMarker(VkLatencyMarkerNV marker, uint64_t id);
    size_t replayIndex() const;
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
    std::vector<double> replayTimes_;
    bdex::replay::Clock replayClock_;
    FILE* replayLog_ = nullptr;
    double replayShiftMs_ = 0.0;
    uint32_t replayLate_ = 0;
    uint32_t replayW_ = 0, replayH_ = 0;
    VkBuffer replayBuf_[kInFlight]{};
    VkDeviceMemory replayBufMem_[kInFlight]{};
    void* replayMapped_[kInFlight]{};
    VkImage replayImage_[kInFlight]{};
    VkDeviceMemory replayImageMem_[kInFlight]{};
    // --present-thread: per slot, the frame's own image, its rendering and its
    // blit to the swapchain.
    static constexpr int kPtSlots = 8;
    struct PtFrame { int slot; double flashInput; uint64_t id; };
    struct PtWait { uint64_t presentId; int slot; };
    VkRenderPass ptPass_ = VK_NULL_HANDLE;
    VkCommandPool ptPool_ = VK_NULL_HANDLE;  // the present thread's (a pool is used by one thread)
    VkImage ptImage_[kPtSlots]{};
    VkDeviceMemory ptMem_[kPtSlots]{};
    VkImageView ptView_[kPtSlots]{};
    VkFramebuffer ptFb_[kPtSlots]{};
    VkCommandBuffer ptRender_[kPtSlots]{}, ptBlit_[kPtSlots]{};
    VkFence ptRenderFence_[kPtSlots]{}, ptBlitFence_[kPtSlots]{};
    VkSemaphore ptAcquire_[kPtSlots]{};
    int ptSlot_ = 0;
    bool ptHasPresentWait_ = false;  // VK_KHR_present_id + VK_KHR_present_wait enabled
    PFN_vkWaitForPresentKHR ptWaitForPresent_ = nullptr;
    uint64_t ptPresentId_ = 0;        // present thread
    std::thread ptQueueThread_, ptWaitThread_;
    std::mutex ptMutex_;
    std::condition_variable ptCv_;
    std::deque<PtFrame> ptFrames_;
    std::deque<PtWait> ptWaits_;
    int ptCredits_ = 0;    // the frame-latency semaphore (L - 1 at the start)
    int ptInFlight_ = 0;   // frames handed to the present thread, not released yet
    bool ptStop_ = false;
    std::atomic<bool> ptRecreate_{false};
    // --sim-thread: the frames the game thread hands over (input time, id)
    // and how far the render thread got (FFrameEndSync).
    struct SimFrame { double flashInput; uint64_t id; };
    std::thread simThread_;
    std::deque<SimFrame> simFrames_;   // ptMutex_
    uint64_t renderDone_ = 0;          // ptMutex_: the last frame the render thread finished
    // --reflex
    VkSemaphore reflexSem_ = VK_NULL_HANDLE;  // timeline, signalled by vkLatencySleepNV
    uint64_t reflexValue_ = 0;
    uint32_t reflexTimeouts_ = 0;
    PFN_vkLatencySleepNV latencySleep_ = nullptr;
    PFN_vkSetLatencySleepModeNV setSleepMode_ = nullptr;
    PFN_vkSetLatencyMarkerNV setMarker_ = nullptr;
    bool ptFifo_ = false;  // the swapchain presents in FIFO: the present thread waits for each present (with present wait)
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
    app.apiVersion = opt_.reflex ? VK_API_VERSION_1_2 : VK_API_VERSION_1_1;  // (--reflex: timeline semaphores)
    const std::string engine = !opt_.engine.empty() ? opt_.engine : opt_.presentThread ? "vkd3d" : "";
    if (!engine.empty()) app.pEngineName = engine.c_str();
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
    std::vector<const char*> devExts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    // --present-thread: present ids and waits where the device has them, as
    // DXVK and VKD3D-Proton enable them.
    VkPhysicalDevicePresentIdFeaturesKHR presentIdFeat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    VkPhysicalDevicePresentWaitFeaturesKHR presentWaitFeat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
    if (opt_.presentThread) {
        uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(physDev_, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> exts(n);
        vkEnumerateDeviceExtensionProperties(physDev_, nullptr, &n, exts.data());
        bool id = false, wait = false;
        for (const auto& e : exts) {
            id = id || strcmp(e.extensionName, VK_KHR_PRESENT_ID_EXTENSION_NAME) == 0;
            wait = wait || strcmp(e.extensionName, VK_KHR_PRESENT_WAIT_EXTENSION_NAME) == 0;
        }
        if (id && wait) {
            presentIdFeat.pNext = &presentWaitFeat;
            VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            f2.pNext = &presentIdFeat;
            vkGetPhysicalDeviceFeatures2(physDev_, &f2);
            ptHasPresentWait_ = presentIdFeat.presentId && presentWaitFeat.presentWait;
        }
        if (ptHasPresentWait_) {
            devExts.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
            devExts.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
            presentWaitFeat.pNext = nullptr;
            dci.pNext = &presentIdFeat;
        }
        printf("demo: present thread, frame latency %d, present wait %s\n", opt_.presentThread,
               ptHasPresentWait_ ? "available" : "not offered");
    }
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
        antiLagFeature.pNext = const_cast<void*>(dci.pNext);
        dci.pNext = &antiLagFeature;
        devExts.push_back(VK_AMD_ANTI_LAG_EXTENSION_NAME);
    }
#else
    if (opt_.antiLag) throw std::runtime_error("--anti-lag: built with Vulkan headers older than VK_AMD_anti_lag");
#endif
    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    if (opt_.reflex) {
        uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(physDev_, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> exts(n);
        vkEnumerateDeviceExtensionProperties(physDev_, nullptr, &n, exts.data());
        bool found = false;
        for (const auto& e : exts) found = found || strcmp(e.extensionName, VK_NV_LOW_LATENCY_2_EXTENSION_NAME) == 0;
        if (!found) throw std::runtime_error("--reflex: VK_NV_low_latency2 not available");
        devExts.push_back(VK_NV_LOW_LATENCY_2_EXTENSION_NAME);
        timelineFeat.timelineSemaphore = VK_TRUE;
        timelineFeat.pNext = const_cast<void*>(dci.pNext);
        dci.pNext = &timelineFeat;
    }
    dci.enabledExtensionCount = static_cast<uint32_t>(devExts.size());
    dci.ppEnabledExtensionNames = devExts.data();
    VK_CHECK(vkCreateDevice(physDev_, &dci, nullptr, &device_));
    if (opt_.reflex) {
        latencySleep_ = reinterpret_cast<PFN_vkLatencySleepNV>(vkGetDeviceProcAddr(device_, "vkLatencySleepNV"));
        setSleepMode_ = reinterpret_cast<PFN_vkSetLatencySleepModeNV>(vkGetDeviceProcAddr(device_, "vkSetLatencySleepModeNV"));
        setMarker_ = reinterpret_cast<PFN_vkSetLatencyMarkerNV>(vkGetDeviceProcAddr(device_, "vkSetLatencyMarkerNV"));
        if (!latencySleep_ || !setSleepMode_ || !setMarker_) throw std::runtime_error("--reflex: no vkLatencySleepNV");
        VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        sci.pNext = &ti;
        VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &reflexSem_));
        printf("demo: Reflex (VK_NV_low_latency2) on the game thread\n");
    }
    if (ptHasPresentWait_)
        ptWaitForPresent_ = reinterpret_cast<PFN_vkWaitForPresentKHR>(vkGetDeviceProcAddr(device_, "vkWaitForPresentKHR"));
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
    if (opt_.presentThread) {
        ptCreateTargets();
        ptCredits_ = opt_.presentThread - 1;
        ptQueueThread_ = std::thread([this] { ptQueueLoop(); });
        ptWaitThread_ = std::thread([this] { ptWaitLoop(); });
        if (opt_.simThread) simThread_ = std::thread([this] { simLoop(); });
    }
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
    else if (opt_.presentThread && opt_.mode == VK_PRESENT_MODE_IMMEDIATE_KHR &&
             std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end())
        mode = VK_PRESENT_MODE_MAILBOX_KHR;  // (as VKD3D-Proton and DXVK pick for a game without vertical sync)
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
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     (opt_.replayDir.empty() && !opt_.presentThread ? VkImageUsageFlags(0) : VkImageUsageFlags(VK_IMAGE_USAGE_TRANSFER_DST_BIT));
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
    if (setSleepMode_) {
        VkLatencySleepModeInfoNV lm{VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV};
        lm.lowLatencyMode = VK_TRUE;
        if (opt_.reflexLimit) lm.minimumIntervalUs = uint32_t(1e6 / opt_.fps);
        setSleepMode_(device_, swapchain_, &lm);
    }
    ptFifo_ = mode == VK_PRESENT_MODE_FIFO_KHR || mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR;

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
        // --gpu-load: the scene drawn first, all overwritten by the
        // recorded frame — a game as heavy on the GPU, on real content.
        if (opt_.gpuLoad > 1) {
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
            PushConstants pc{{float(extent_.width), float(extent_.height)}, 0.f, frameCount_, 0u};
            vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
            for (long i = 0; i < opt_.gpuLoad; ++i) vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRenderPass(cmd);
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                 1, &mb, 0, nullptr, 0, nullptr);
        }
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

    VkPipelineStageFlags wait = opt_.replayDir.empty() ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                : opt_.gpuLoad > 1     ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT
                                                       : VK_PIPELINE_STAGE_TRANSFER_BIT;
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
    double replayTarget = 0.0, replayActual = 0.0, replayOffset = 0.0;
    if (opt_.replayTiming) {
        replayOffset = replayTimes_[replayIndex()] - replayTimes_[size_t(opt_.replayStart)];
        replayTarget = replayClock_.target(replayOffset, steadyMs());
        using clock = std::chrono::steady_clock;
        std::this_thread::sleep_until(clock::time_point(std::chrono::duration_cast<clock::duration>(
            std::chrono::duration<double, std::milli>(replayTarget))));
        replayActual = steadyMs();
    }
    r = vkQueuePresentKHR(queue_, &pi);
    if (opt_.replayTiming) {
        const double shift = replayClock_.commit(replayOffset, replayActual);
        replayShiftMs_ += shift;
        replayLate_ += shift > 0.0;
        if (replayLog_ && fprintf(replayLog_, "%s,%.3f,%.3f,%.3f,%.3f\n",
                                 replayFrames_[replayIndex()].filename().string().c_str(), replayOffset,
                                 replayTarget, replayActual, shift) < 0)
            throw std::runtime_error("--replay-log: write failed");
    }
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
    if (opt_.replayTiming) {
        if (size_t(opt_.replayStart) >= replayFrames_.size())
            throw std::runtime_error("--replay-timing: start is past the last source frame");
        std::ifstream times(std::filesystem::path(opt_.replayDir) / "timestamps.csv");
        if (!times) throw std::runtime_error("--replay-timing: timestamps.csv missing; this recording has no source clock");
        std::vector<std::string> names;
        for (const auto& p : replayFrames_) names.push_back(p.filename().string());
        replayTimes_ = bdex::replay::readTimes(times, names);
        if (!opt_.replayLog.empty()) {
            // Never overwrite a recording (including its PPMs or timestamps).
            if (std::filesystem::exists(opt_.replayLog)) throw std::runtime_error("--replay-log: file already exists");
            replayLog_ = fopen(opt_.replayLog.c_str(), "w");
            if (!replayLog_ || fprintf(replayLog_, "frame,source_ms,target_ms,present_ms,shift_ms\n") < 0)
                throw std::runtime_error("--replay-log: cannot create log");
        }
        printf("demo: recorded source timing (not --fps), stops at the last source; late frames shift following deadlines\n");
    }
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
    const size_t idx = replayIndex();
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

size_t Demo::replayIndex() const {
    return std::min(replayFrames_.size() - 1, size_t(opt_.replayStart) + size_t(frameCount_) * size_t(opt_.replayStep));
}

// --present-thread: the frame's own image, rendered in a pass compatible with
// the pipeline's (only its final layout differs: the present thread's blit
// moves it to TRANSFER_SRC).
void Demo::ptCreateTargets() {
    if (!ptPass_) {
        VkAttachmentDescription att{};
        att.format = format_;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkSubpassDependency dep{};  // (as renderPass_'s: render passes are compatible only with identical dependencies)
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
        VK_CHECK(vkCreateRenderPass(device_, &rci, nullptr, &ptPass_));
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = queueFamily_;
        VK_CHECK(vkCreateCommandPool(device_, &pci, nullptr, &ptPool_));
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = kPtSlots;
        VK_CHECK(vkAllocateCommandBuffers(device_, &cai, ptRender_));
        cai.commandPool = ptPool_;
        VK_CHECK(vkAllocateCommandBuffers(device_, &cai, ptBlit_));
        for (int i = 0; i < kPtSlots; ++i) {
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            VK_CHECK(vkCreateFence(device_, &fi, nullptr, &ptRenderFence_[i]));
            VK_CHECK(vkCreateFence(device_, &fi, nullptr, &ptBlitFence_[i]));
            VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VK_CHECK(vkCreateSemaphore(device_, &si, nullptr, &ptAcquire_[i]));
        }
    }
    for (int i = 0; i < kPtSlots; ++i) {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = format_;
        ici.extent = {extent_.width, extent_.height, 1};
        ici.mipLevels = ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VK_CHECK(vkCreateImage(device_, &ici, nullptr, &ptImage_[i]));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, ptImage_[i], &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = findMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &ptMem_[i]));
        VK_CHECK(vkBindImageMemory(device_, ptImage_[i], ptMem_[i], 0));
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = ptImage_[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format_;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &ptView_[i]));
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = ptPass_;
        fci.attachmentCount = 1;
        fci.pAttachments = &ptView_[i];
        fci.width = extent_.width;
        fci.height = extent_.height;
        fci.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device_, &fci, nullptr, &ptFb_[i]));
    }
}

void Demo::ptDestroyTargets() {
    for (int i = 0; i < kPtSlots; ++i) {
        if (ptFb_[i]) vkDestroyFramebuffer(device_, ptFb_[i], nullptr);
        if (ptView_[i]) vkDestroyImageView(device_, ptView_[i], nullptr);
        if (ptImage_[i]) vkDestroyImage(device_, ptImage_[i], nullptr);
        if (ptMem_[i]) vkFreeMemory(device_, ptMem_[i], nullptr);
        ptFb_[i] = VK_NULL_HANDLE;
        ptView_[i] = VK_NULL_HANDLE;
        ptImage_[i] = VK_NULL_HANDLE;
        ptMem_[i] = VK_NULL_HANDLE;
    }
}

// Every frame handed over released (a new swapchain, the end).
void Demo::ptDrain() {
    std::unique_lock<std::mutex> lk(ptMutex_);
    ptCv_.wait_for(lk, std::chrono::seconds(2), [this] { return ptInFlight_ == 0; });
}

void Demo::ptStopThreads() {
    if (!ptQueueThread_.joinable()) return;
    ptDrain();
    {
        std::lock_guard<std::mutex> lk(ptMutex_);
        ptStop_ = true;
    }
    ptCv_.notify_all();
    if (simThread_.joinable()) simThread_.join();
    ptQueueThread_.join();
    ptWaitThread_.join();
}

void Demo::reflexMarker(VkLatencyMarkerNV marker, uint64_t id) {
    if (!setMarker_) return;
    VkSetLatencyMarkerInfoNV mi{VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV};
    mi.presentID = id;
    mi.marker = marker;
    setMarker_(device_, swapchain_, &mi);
}

// --sim-thread: the game thread — its own cap or Reflex's sleep, then the
// input and the simulation (4 ms), handed to the render thread; it begins
// the next frame once the render thread has finished the one before this one
// (one frame of lag, as Unreal's FFrameEndSync allows).
void Demo::simLoop() {
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / opt_.fps));
    auto next = clock::now();
    for (uint64_t id = 1;; ++id) {
        if (opt_.reflex) {
            VkLatencySleepInfoNV si{VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV};
            si.signalSemaphore = reflexSem_;
            si.value = ++reflexValue_;
            latencySleep_(device_, swapchain_, &si);
            VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wi.semaphoreCount = 1;
            wi.pSemaphores = &reflexSem_;
            wi.pValues = &si.value;
            if (vkWaitSemaphores(device_, &wi, 200'000'000) == VK_TIMEOUT) ++reflexTimeouts_;
        } else if (!opt_.uncapped) {
            next += period;
            if (next > clock::now()) std::this_thread::sleep_until(next);
            else next = clock::now();
        }
        reflexMarker(VK_LATENCY_MARKER_SIMULATION_START_NV, id);
        double flashInput = -1.0;
        if (opt_.latencyTest == 2 && steadyMs() >= nextAutoMs_) {
            if (nextAutoMs_ > 0.0) flashInput = steadyMs();  // (none in the first half second)
            nextAutoMs_ = steadyMs() + 500.0;
        }
        const auto busy = clock::now() + std::chrono::milliseconds(4);
        while (clock::now() < busy) {}
        reflexMarker(VK_LATENCY_MARKER_SIMULATION_END_NV, id);
        std::unique_lock<std::mutex> lk(ptMutex_);
        simFrames_.push_back({flashInput, id});
        ptCv_.notify_all();
        ptCv_.wait(lk, [&] { return ptStop_ || renderDone_ + 1 >= id; });
        if (ptStop_) return;
    }
}

// The game's thread under --present-thread: reads its input, renders into
// the frame's own image and hands the frame over — then, in Present(), takes
// a frame-latency credit (one per released present; L - 1 at the start), so
// that frame k + 1 begins once present k - L + 1 is done.
void Demo::drawFramePT() {
    static const auto start = std::chrono::steady_clock::now();
    if (ptRecreate_.exchange(false)) {
        ptDrain();
        vkDeviceWaitIdle(device_);
        ptDestroyTargets();
        destroySwapchain();
        createSwapchain();
        ptCreateTargets();
    }
    const int slot = ptSlot_;
    ptSlot_ = (ptSlot_ + 1) % kPtSlots;
    VkFence done[2] = {ptRenderFence_[slot], ptBlitFence_[slot]};  // (its last use, kPtSlots frames back)
    vkWaitForFences(device_, 2, done, VK_TRUE, UINT64_MAX);

    if (opt_.deterministic) animTime_ = frameCount_ / opt_.fps;
    else animTime_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    PushConstants pc{{float(extent_.width), float(extent_.height)}, float(animTime_ * opt_.speed), frameCount_,
                     opt_.cutEvery > 0 ? uint32_t((frameCount_ / opt_.cutEvery) & 1) : 0u};
    double flashInput = -1.0;
    uint64_t id = frameCount_ + 1;
    if (opt_.simThread) {
        std::unique_lock<std::mutex> lk(ptMutex_);
        ptCv_.wait(lk, [this] { return ptStop_ || !simFrames_.empty(); });
        if (simFrames_.empty()) return;
        flashInput = simFrames_.front().flashInput;
        id = simFrames_.front().id;
        simFrames_.pop_front();
        if (flashInput >= 0.0) pc.variant = 2u;
    } else {
        if (opt_.latencyTest == 2 && steadyMs() >= nextAutoMs_) {
            if (nextAutoMs_ > 0.0) inputMs_ = steadyMs();  // (none in the first half second)
            nextAutoMs_ = steadyMs() + 500.0;
        }
        if (opt_.latencyTest && inputMs_ >= 0.0) {
            flashInput = inputMs_;
            inputMs_ = -1.0;
            pc.variant = 2u;
        }
    }
    reflexMarker(VK_LATENCY_MARKER_RENDERSUBMIT_START_NV, id);
    VkCommandBuffer cmd = ptRender_[slot];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = ptPass_;
    rbi.framebuffer = ptFb_[slot];
    rbi.renderArea = {{0, 0}, extent_};
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{0.f, 0.f, float(extent_.width), float(extent_.height), 0.f, 1.f};
    VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    for (long i = 0; i < opt_.gpuLoad; ++i) vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
    {
        std::lock_guard<std::mutex> lk(ptMutex_);
        ptFrames_.push_back({slot, flashInput, id});
        ++ptInFlight_;
    }
    ptCv_.notify_all();
    {
        std::unique_lock<std::mutex> lk(ptMutex_);
        ptCv_.wait(lk, [this] { return ptCredits_ > 0 || ptStop_; });
        --ptCredits_;
        renderDone_ = id;  // (--sim-thread: the game thread may go on)
    }
    ptCv_.notify_all();
    reflexMarker(VK_LATENCY_MARKER_RENDERSUBMIT_END_NV, id);
    ++frameCount_;
}

// The present thread (DXVK's submission thread, VKD3D-Proton's queue
// thread): submits each frame's rendering, acquires a swapchain image, blits
// the frame to it and presents, in order — with a present id in FIFO where
// the device has present wait, as both do.
void Demo::ptQueueLoop() {
    for (;;) {
        PtFrame f{};
        {
            std::unique_lock<std::mutex> lk(ptMutex_);
            ptCv_.wait(lk, [this] { return ptStop_ || !ptFrames_.empty(); });
            if (ptFrames_.empty()) return;
            f = ptFrames_.front();
            ptFrames_.pop_front();
        }
        vkResetFences(device_, 1, &ptRenderFence_[f.slot]);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &ptRender_[f.slot];
        VK_CHECK(vkQueueSubmit(queue_, 1, &si, ptRenderFence_[f.slot]));
        uint32_t img = 0;
        uint64_t id = 0;
        VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, ptAcquire_[f.slot], VK_NULL_HANDLE, &img);
        if (r >= 0) {
            VkCommandBuffer cmd = ptBlit_[f.slot];
            vkResetCommandBuffer(cmd, 0);
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &bi);
            VkImageMemoryBarrier b[2]{};
            for (auto& x : b) {
                x.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                x.srcQueueFamilyIndex = x.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                x.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            }
            b[0].image = ptImage_[f.slot];
            b[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            b[1].image = images_[img];
            b[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
            VkImageBlit blit{};
            blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = blit.dstOffsets[1] = {int32_t(extent_.width), int32_t(extent_.height), 1};
            vkCmdBlitImage(cmd, ptImage_[f.slot], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images_[img],
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
            b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b[1].dstAccessMask = 0;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                                 nullptr, 1, &b[1]);
            vkEndCommandBuffer(cmd);
            const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo bs{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            bs.waitSemaphoreCount = 1;
            bs.pWaitSemaphores = &ptAcquire_[f.slot];
            bs.pWaitDstStageMask = &wait;
            bs.commandBufferCount = 1;
            bs.pCommandBuffers = &cmd;
            bs.signalSemaphoreCount = 1;
            bs.pSignalSemaphores = &renderSem_[img];
            vkResetFences(device_, 1, &ptBlitFence_[f.slot]);
            VK_CHECK(vkQueueSubmit(queue_, 1, &bs, ptBlitFence_[f.slot]));
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &renderSem_[img];
            pi.swapchainCount = 1;
            pi.pSwapchains = &swapchain_;
            pi.pImageIndices = &img;
            VkPresentIdKHR pid{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
            if (ptHasPresentWait_ && ptFifo_) {
                id = ++ptPresentId_;
                pid.swapchainCount = 1;
                pid.pPresentIds = &id;
                pi.pNext = &pid;
            }
            if (f.flashInput >= 0.0 && latencyLog_) {
                fprintf(latencyLog_, "flash %.3f %.3f\n", f.flashInput, steadyMs());
                fflush(latencyLog_);
            }
            reflexMarker(VK_LATENCY_MARKER_PRESENT_START_NV, f.id);
            r = vkQueuePresentKHR(queue_, &pi);
            reflexMarker(VK_LATENCY_MARKER_PRESENT_END_NV, f.id);
            if (r < 0) id = 0;
        }
        if (r == VK_ERROR_OUT_OF_DATE_KHR || (r == VK_SUBOPTIMAL_KHR && !opt_.ignoreResize)) ptRecreate_ = true;
        else if (r < 0) fprintf(stderr, "demo: present thread: %d\n", r);
        {
            std::lock_guard<std::mutex> lk(ptMutex_);
            ptWaits_.push_back({id, f.slot});
        }
        ptCv_.notify_all();
    }
}

// Releases each present in turn, as DXVK's frame thread and VKD3D-Proton's
// swapchain-sync thread do: after vkWaitForPresentKHR for a present with an
// id (FIFO), else once its blit is done on the GPU.
void Demo::ptWaitLoop() {
    for (;;) {
        PtWait w{};
        {
            std::unique_lock<std::mutex> lk(ptMutex_);
            ptCv_.wait(lk, [this] { return ptStop_ || !ptWaits_.empty(); });
            if (ptWaits_.empty()) return;
            w = ptWaits_.front();
            ptWaits_.pop_front();
        }
        if (w.presentId && ptWaitForPresent_) ptWaitForPresent_(device_, swapchain_, w.presentId, UINT64_MAX);
        else vkWaitForFences(device_, 1, &ptBlitFence_[w.slot], VK_TRUE, UINT64_MAX);
        {
            std::lock_guard<std::mutex> lk(ptMutex_);
            ++ptCredits_;
            --ptInFlight_;
        }
        ptCv_.notify_all();
    }
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
        if (opt_.presentThread) drawFramePT();
        else drawFrame();
        ++statFrames;
        if (opt_.replayTiming && frameCount_ > (replayFrames_.size() - 1 - size_t(opt_.replayStart)) / size_t(opt_.replayStep)) break;
        if (opt_.frames >= 0 && frameCount_ >= static_cast<uint32_t>(opt_.frames)) break;
        if (opt_.duration > 0.0 && std::chrono::duration<double>(clock::now() - runStart).count() >= opt_.duration) break;
        const auto now = clock::now();
        if (std::chrono::duration<double>(now - statT).count() >= 2.0) {
            char title[128];
            if (opt_.replayTiming)
                snprintf(title, sizeof(title), "bdex demo - %.1f fps rendered (recorded source timing)",
                         statFrames / std::chrono::duration<double>(now - statT).count());
            else if (opt_.uncapped)
                snprintf(title, sizeof(title), "bdex demo - %.1f fps rendered (uncapped)",
                         statFrames / std::chrono::duration<double>(now - statT).count());
            else
                snprintf(title, sizeof(title), "bdex demo - %.1f fps rendered (target %.0f)",
                         statFrames / std::chrono::duration<double>(now - statT).count(), opt_.fps);
            glfwSetWindowTitle(window_, title);
            printf("%s\n", title);
            statT = now;
            statFrames = 0;
        }
        if (!opt_.replayTiming && !opt_.uncapped && !opt_.simThread) {
            next += period;
            if (next > clock::now()) std::this_thread::sleep_until(next);
            else next = clock::now();
        }
    }
    vkDeviceWaitIdle(device_);
    if (opt_.replayTiming) {
        printf("demo: replayed %u sources; %u late frames shifted deadlines by %.3f ms total\n",
               frameCount_, replayLate_, replayShiftMs_);
        if (replayLog_) {
            const int result = fclose(replayLog_);
            replayLog_ = nullptr;
            if (result != 0) throw std::runtime_error("--replay-log: flush failed");
        }
    }
}

void Demo::cleanup() {
    ptStopThreads();
    if (device_) {
        vkDeviceWaitIdle(device_);
        ptDestroyTargets();
        for (int i = 0; i < kPtSlots; ++i) {
            if (ptRenderFence_[i]) vkDestroyFence(device_, ptRenderFence_[i], nullptr);
            if (ptBlitFence_[i]) vkDestroyFence(device_, ptBlitFence_[i], nullptr);
            if (ptAcquire_[i]) vkDestroySemaphore(device_, ptAcquire_[i], nullptr);
        }
        if (ptPass_) vkDestroyRenderPass(device_, ptPass_, nullptr);
        if (reflexSem_) vkDestroySemaphore(device_, reflexSem_, nullptr);
        if (reflexTimeouts_) printf("demo: %u Reflex sleep(s) never signalled within 200 ms\n", reflexTimeouts_);
        if (ptPool_) vkDestroyCommandPool(device_, ptPool_, nullptr);
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
    if (replayLog_) fclose(replayLog_);
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
        else if (a == "--uncapped") o.uncapped = true;
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
        else if (a == "--present-thread") o.presentThread = std::clamp(atoi(value().c_str()), 1, 6);
        else if (a == "--engine") o.engine = value();
        else if (a == "--sim-thread") o.simThread = true;
        else if (a == "--reflex") o.reflex = true;
        else if (a == "--reflex-limit") o.reflexLimit = true;
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
        else if (a == "--replay-timing") o.replayTiming = true;
        else if (a == "--replay-log") o.replayLog = value();
        else if (a == "-h" || a == "--help") {
            printf("usage: bdex_demo [--fps N | --uncapped] [--frames N] [--size WxH] [--mode fifo|mailbox|immediate]\n"
                   "                 [--deterministic] [--ignore-resize] [--speed X] [--srgb] [--cut N] [--repeat N] [--duration S]\n"
                   "                 [--gpu-load N] [--latency-test [auto]] [--latency-log FILE] [--anti-lag]\n"
                   "                 [--present-thread L [--engine NAME] [--sim-thread [--reflex [--reflex-limit]]]]\n"
                   "                 [--stress resize|recreate|surface|window|fullscreen|minimize:N[,...]] [--stress-until N]\n"
                   "                 [--replay DIR [--replay-step N] [--replay-start N]\n"
                   "                  [--replay-timing [--replay-log FILE]]]\n");
            exit(0);
        } else {
            throw std::runtime_error("unknown option " + a);
        }
    }
    if (o.replayTiming && o.replayDir.empty()) throw std::runtime_error("--replay-timing needs --replay DIR");
    if ((o.simThread || o.reflex) && !o.presentThread) throw std::runtime_error("--sim-thread, --reflex need --present-thread");
    if (o.reflex && !o.simThread) throw std::runtime_error("--reflex needs --sim-thread (Reflex sleeps on the game thread)");
    if (o.reflexLimit && (!o.reflex || o.uncapped)) throw std::runtime_error("--reflex-limit needs --reflex and a cap (--fps)");
    if (o.presentThread && (!o.replayDir.empty() || o.repeat > 1 || o.antiLag))
        throw std::runtime_error("--present-thread does not go with --replay, --repeat or --anti-lag");
    for (long every : o.stressEvery)
        if (o.presentThread && every > 0) throw std::runtime_error("--present-thread does not go with --stress");
    if (!o.replayLog.empty() && !o.replayTiming) throw std::runtime_error("--replay-log needs --replay-timing");
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
