#include "framegen.h"
#include "frame_order.h"
#include "log.h"
#include "online_tune.h"
#include "status.h"
#include "vk_util.h"

#include "presenter.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace bdex {

#define VKC(x)                                                             \
    do {                                                                   \
        VkResult r_ = (x);                                                 \
        if (r_ != VK_SUCCESS) {                                            \
            BDEX_ERR("%s failed (%s)", #x, vkResultName(r_));              \
            return false;                                                  \
        }                                                                  \
    } while (0)

namespace {

bool hasExt(const char* name, const std::vector<VkExtensionProperties>& list) {
    for (const auto& e : list)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct Gpu {
    VkPhysicalDevice pd;
    uint32_t qf;
    VkPhysicalDeviceProperties props;
    bool hostImport;
};

// The GPUs that can present to the surface with a GRAPHICS+COMPUTE queue.
std::vector<Gpu> usableGpus(VkInstance inst, VkSurfaceKHR surface) {
    std::vector<Gpu> out;
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> list(n);
    vkEnumeratePhysicalDevices(inst, &n, list.data());
    for (VkPhysicalDevice pd : list) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd, &p);
        if (p.apiVersion < VK_API_VERSION_1_1) continue;
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> exts(ne);
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, exts.data());
        if (!hasExt(VK_KHR_SWAPCHAIN_EXTENSION_NAME, exts)) continue;
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qs(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qs.data());
        uint32_t qf = UINT32_MAX;
        for (uint32_t i = 0; i < nq && qf == UINT32_MAX; ++i) {
            VkBool32 ok = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &ok);
            if (ok && (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) qf = i;
        }
        if (qf == UINT32_MAX) continue;
        out.push_back({pd, qf, p, hasExt(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, exts)});
    }
    return out;
}

const char* kindName(VkPhysicalDeviceType t) {
    static const char* k[] = {"other", "integrated", "discrete", "virtual", "software"};
    return k[t <= 4 ? t : 0];
}

bool makeInstance(VkInstance& inst) {
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, exts.data());
    const char* want[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XLIB_SURFACE_EXTENSION_NAME};
    for (const char* e : want)
        if (!hasExt(e, exts)) {
            BDEX_ERR("instance extension %s missing", e);
            return false;
        }
    std::vector<const char*> layers;
    if (std::getenv("BDEX_CAP_VALIDATION")) layers.push_back("VK_LAYER_KHRONOS_validation");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "bdex-capture";  // (the layer stays out of it: a non-game, platform.cpp)
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = want;
    ici.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    VKC(vkCreateInstance(&ici, nullptr, &inst));
    return true;
}

} // namespace

Presenter::Presenter() = default;
Presenter::~Presenter() { shutdown(); }

std::vector<std::string> Presenter::listGpus(Display* dpy, Window overlay) {
    std::vector<std::string> out;
    VkInstance inst = VK_NULL_HANDLE;
    if (!makeInstance(inst)) return out;
    VkXlibSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR};
    sci.dpy = dpy;
    sci.window = overlay;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (vkCreateXlibSurfaceKHR(inst, &sci, nullptr, &surface) == VK_SUCCESS) {
        for (const auto& g : usableGpus(inst, surface))
            out.push_back(std::string(g.props.deviceName) + " (" + kindName(g.props.deviceType) + ")");
        vkDestroySurfaceKHR(inst, surface, nullptr);
    }
    vkDestroyInstance(inst, nullptr);
    return out;
}

bool Presenter::init(Display* dpy, Window overlay, const std::string& gpu, bool preferFifo, const Config& cfg) {
    preferFifo_ = preferFifo;
    if (!makeInstance(inst_)) return false;
    VkXlibSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR};
    sci.dpy = dpy;
    sci.window = overlay;
    VKC(vkCreateXlibSurfaceKHR(inst_, &sci, nullptr, &surface_));

    const auto gpus = usableGpus(inst_, surface_);
    if (gpus.empty()) {
        BDEX_ERR("no GPU can present to the overlay window");
        return false;
    }
    for (size_t i = 0; i < gpus.size(); ++i)
        BDEX_INFO("gpu %zu: %s (%s%s)", i, gpus[i].props.deviceName, kindName(gpus[i].props.deviceType),
                  gpus[i].hostImport ? ", host memory import" : "");
    size_t pick = 0;
    if (!gpu.empty() && std::all_of(gpu.begin(), gpu.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
        pick = std::stoul(gpu);
        if (pick >= gpus.size()) {
            BDEX_ERR("--gpu %s: only %zu usable GPU(s)", gpu.c_str(), gpus.size());
            return false;
        }
    } else if (!gpu.empty()) {
        pick = gpus.size();
        for (size_t i = 0; i < gpus.size() && pick == gpus.size(); ++i)
            if (lower(gpus[i].props.deviceName).find(lower(gpu)) != std::string::npos) pick = i;
        if (pick == gpus.size()) {
            BDEX_ERR("--gpu %s: no GPU of that name", gpu.c_str());
            return false;
        }
    } else {
        auto score = [](VkPhysicalDeviceType t) {
            return t == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : t == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                   : t == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? 1 : 0;
        };
        for (size_t i = 1; i < gpus.size(); ++i)
            if (score(gpus[i].props.deviceType) > score(gpus[pick].props.deviceType)) pick = i;
    }
    const Gpu& g = gpus[pick];
    phys_ = g.pd;
    qf_ = g.qf;
    gpuName_ = g.props.deviceName;
    hostImportExt_ = g.hostImport && !std::getenv("BDEX_CAP_NO_HOST_IMPORT");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo dq{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    dq.queueFamilyIndex = qf_;
    dq.queueCount = 1;
    dq.pQueuePriorities = &prio;
    std::vector<const char*> exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (hostImportExt_) {
        exts.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
        exts.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    }
    VkPhysicalDeviceFeatures feats{};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &dq;
    dci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    dci.ppEnabledExtensionNames = exts.data();
    dci.pEnabledFeatures = &feats;
    VKC(vkCreateDevice(phys_, &dci, nullptr, &dev_));
    vkGetDeviceQueue(dev_, qf_, 0, &queue_);
    if (hostImportExt_) {
        getHostProps_ = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
            vkGetDeviceProcAddr(dev_, "vkGetMemoryHostPointerPropertiesEXT"));
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &hp};
        vkGetPhysicalDeviceProperties2(phys_, &p2);
        hostAlign_ = std::max<VkDeviceSize>(hp.minImportedHostPointerAlignment, 1);
        if (!getHostProps_) hostImportExt_ = false;
    }

    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = qf_;
    VKC(vkCreateCommandPool(dev_, &cp, nullptr, &cpool_));
    VkCommandBufferAllocateInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cb.commandPool = cpool_;
    cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb.commandBufferCount = 1;
    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fen{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fen.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (auto& s : cmds_) {
        VKC(vkAllocateCommandBuffers(dev_, &cb, &s.cmd));
        VKC(vkCreateFence(dev_, &fen, nullptr, &s.fence));
        VKC(vkCreateSemaphore(dev_, &sem, nullptr, &s.acquireSem));
    }

    // The layer's DeviceData over our plain device (as the Windows capture's
    // VkCtx and bdex_offline do): FrameGen works on it unchanged.
    layerInst_.vt.load(inst_, vkGetInstanceProcAddr);
    layerInst_.instance = inst_;
    layerInst_.apiVersion = VK_API_VERSION_1_1;
    layerInst_.config = cfg;
    layerDev_.vt.load(dev_, vkGetDeviceProcAddr);
    layerDev_.inst = &layerInst_;
    layerDev_.physDev = phys_;
    layerDev_.device = dev_;
    layerDev_.config = cfg;
    layerDev_.props = g.props;
    vkGetPhysicalDeviceMemoryProperties(phys_, &layerDev_.memProps);
    layerDev_.queue = queue_;
    layerDev_.queueFamily = qf_;
    layerDev_.queueIndex = 0;
    layerDev_.layerUsable = true;
    layerDev_.cmdPool = cpool_;
    layerDev_.config.autoConfigure(layerDev_.props, layerDev_.memProps);
    const Config& fc = layerDev_.config;
    BDEX_INFO("using gpu %zu: %s; frame generation x%d %s, preset %s", pick, gpuName_.c_str(), fc.multiplier,
              fc.modeName(), fc.autoTunedTo.empty() ? "(explicit)" : fc.autoTunedTo.c_str());
    return true;
}

// -------------------------------------------------------------- sizing ----

const char* Presenter::presentModeName() const {
    switch (presentMode_) {
    case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo-relaxed";
    default: return "fifo";
    }
}

bool Presenter::fifo() const {
    return presentMode_ == VK_PRESENT_MODE_FIFO_KHR || presentMode_ == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
}

bool Presenter::createSwapchain(uint32_t w, uint32_t h) {
    VkSurfaceCapabilitiesKHR caps;
    VKC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps));
    if (caps.currentExtent.width != UINT32_MAX) {
        w = caps.currentExtent.width;
        h = caps.currentExtent.height;
    }
    if (w == 0 || h == 0) return false;
    width_ = std::clamp(w, caps.minImageExtent.width, caps.maxImageExtent.width);
    height_ = std::clamp(h, caps.minImageExtent.height, caps.maxImageExtent.height);
    uint32_t nModes = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &nModes, nullptr);
    std::vector<VkPresentModeKHR> modes(nModes);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &nModes, modes.data());
    auto have = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    if (!preferFifo_) {
        if (have(VK_PRESENT_MODE_MAILBOX_KHR)) presentMode_ = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (have(VK_PRESENT_MODE_IMMEDIATE_KHR)) presentMode_ = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    uint32_t nFmt = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &nFmt, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(nFmt);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &nFmt, fmts.data());
    bool found = false;
    for (auto& f : fmts)
        if (f.format == scFormat_ && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) found = true;
    if (!found) {
        BDEX_ERR("B8G8R8A8_UNORM surface format not supported");
        return false;
    }
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);
    VkSwapchainCreateInfoKHR sw{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sw.surface = surface_;
    sw.minImageCount = imageCount;
    sw.imageFormat = scFormat_;
    sw.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sw.imageExtent = {width_, height_};
    sw.imageArrayLayers = 1;
    sw.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sw.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sw.preTransform = caps.currentTransform;
    sw.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    sw.presentMode = presentMode_;
    sw.clipped = VK_TRUE;
    VKC(vkCreateSwapchainKHR(dev_, &sw, nullptr, &swapchain_));
    uint32_t nImg = 0;
    vkGetSwapchainImagesKHR(dev_, swapchain_, &nImg, nullptr);
    scImages_.resize(nImg);
    vkGetSwapchainImagesKHR(dev_, swapchain_, &nImg, scImages_.data());
    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semDone_.assign(nImg, VK_NULL_HANDLE);
    for (auto& s : semDone_) VKC(vkCreateSemaphore(dev_, &sem, nullptr, &s));
    BDEX_INFO("overlay swapchain %ux%u, %u images, %s", width_, height_, nImg, presentModeName());
    if (captureW_ && (width_ != engineW_ || height_ != engineH_) && !rebuildEngine(captureW_, captureH_)) return false;
    return true;
}

void Presenter::destroySwapchain() {
    if (!dev_) return;
    vkDeviceWaitIdle(dev_);
    for (auto s : semDone_) vkDestroySemaphore(dev_, s, nullptr);
    semDone_.clear();
    if (swapchain_) vkDestroySwapchainKHR(dev_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    scImages_.clear();
    width_ = height_ = 0;
}

bool Presenter::setSize(uint32_t w, uint32_t h) {
    if (!dev_) return false;
    destroySwapchain();
    if (!createSwapchain(w, h)) {
        destroySwapchain();
        return false;
    }
    return true;
}

bool Presenter::sizeChanged() const {
    if (!swapchain_) return true;
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps) != VK_SUCCESS) return false;
    return caps.currentExtent.width != UINT32_MAX &&
           (caps.currentExtent.width != width_ || caps.currentExtent.height != height_);
}

// -------------------------------------------------------------- engine ----

void Presenter::destroyEngine() {
    vkDeviceWaitIdle(dev_);
    fg_.reset();
    for (auto& h : history_)
        if (h.image) layerDev_.destroyImage(h);
    prev_ = -1;
    parity_ = 0;
    engineW_ = engineH_ = 0;
    lastSynthFence_ = lastCopyFence_ = VK_NULL_HANDLE;
}

bool Presenter::rebuildEngine(uint32_t cw, uint32_t ch) {
    destroyEngine();
    captureW_ = cw;
    captureH_ = ch;
    try {
        std::vector<VkImage> sources;
        for (auto& h : history_) {
            h = layerDev_.createImage(scFormat_, {cw, ch},
                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                          VK_IMAGE_USAGE_SAMPLED_BIT,
                                      false);
            sources.push_back(h.image);
        }
        const uint32_t outputs = static_cast<uint32_t>(std::max(1, layerDev_.config.multiplier) - 1);
        fg_ = std::make_unique<FrameGen>(layerDev_, scFormat_, VkExtent2D{cw, ch}, VkExtent2D{width_, height_},
                                         outputs, sources);
    } catch (const std::exception& e) {
        BDEX_ERR("frame generation setup failed: %s", e.what());
        destroyEngine();
        return false;
    }
    engineW_ = width_;
    engineH_ = height_;
    BDEX_INFO("engine: capture %ux%u -> display %ux%u, %d frame(s) per capture", cw, ch, width_, height_,
              std::max(1, layerDev_.config.multiplier));
    return true;
}

Presenter::CmdSlot& Presenter::nextCmd() {
    CmdSlot& s = cmds_[cmdCursor_];
    cmdCursor_ = (cmdCursor_ + 1) % kCmdSlots;
    vkWaitForFences(dev_, 1, &s.fence, VK_TRUE, UINT64_MAX);
    return s;
}

// -------------------------------------------------------------- upload ----

void Presenter::destroyUpload(Upload& u) {
    if (u.lastUse) vkWaitForFences(dev_, 1, &u.lastUse, VK_TRUE, UINT64_MAX);
    if (u.buffer.buffer) vkDestroyBuffer(dev_, u.buffer.buffer, nullptr);
    if (u.buffer.memory) {
        if (u.mapped && !u.imported) vkUnmapMemory(dev_, u.buffer.memory);
        vkFreeMemory(dev_, u.buffer.memory, nullptr);
    }
    u = Upload{};
}

bool Presenter::prepareUpload(Upload& u, const X11Capture::Frame& f, size_t bytes) {
    destroyUpload(u);
    // The MIT-SHM segment itself, as a buffer the GPU reads (no copy). Whole
    // pages, page-aligned (x11_capture.cpp); refused by some drivers for
    // shared memory — then a staging buffer the frame is copied into.
    if (hostImportExt_ && reinterpret_cast<uintptr_t>(f.data) % hostAlign_ == 0) {
        const VkDeviceSize size = (bytes + hostAlign_ - 1) / hostAlign_ * hostAlign_;
        VkMemoryHostPointerPropertiesEXT hp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        VkExternalMemoryBufferCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ext};
        bi.size = size;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VkBuffer buf = VK_NULL_HANDLE;
        if (getHostProps_(dev_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, f.data, &hp) == VK_SUCCESS &&
            vkCreateBuffer(dev_, &bi, nullptr, &buf) == VK_SUCCESS) {
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(dev_, buf, &req);
            const uint32_t bits = hp.memoryTypeBits & req.memoryTypeBits;
            uint32_t type = UINT32_MAX;
            for (uint32_t i = 0; i < layerDev_.memProps.memoryTypeCount && type == UINT32_MAX; ++i)
                if (bits & (1u << i)) type = i;
            VkImportMemoryHostPointerInfoEXT imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
            imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
            imp.pHostPointer = const_cast<uint8_t*>(f.data);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp};
            ai.allocationSize = std::max(size, req.size);
            ai.memoryTypeIndex = type;
            VkDeviceMemory mem = VK_NULL_HANDLE;
            if (type != UINT32_MAX && ai.allocationSize == size && vkAllocateMemory(dev_, &ai, nullptr, &mem) == VK_SUCCESS &&
                vkBindBufferMemory(dev_, buf, mem, 0) == VK_SUCCESS) {
                u.buffer.buffer = buf;
                u.buffer.memory = mem;
                u.buffer.size = size;
                u.imported = true;
                u.generation = f.generation;
                if (!hostImport_) BDEX_INFO("captured frames read in place (host memory import)");
                hostImport_ = true;
                return true;
            }
            if (mem) vkFreeMemory(dev_, mem, nullptr);
            vkDestroyBuffer(dev_, buf, nullptr);
        }
        BDEX_INFO("host memory import refused: captured frames copied");
        hostImportExt_ = false;
    }
    try {
        u.buffer = layerDev_.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    } catch (const std::exception& e) {
        BDEX_ERR("staging buffer: %s", e.what());
        return false;
    }
    if (vkMapMemory(dev_, u.buffer.memory, 0, VK_WHOLE_SIZE, 0, &u.mapped) != VK_SUCCESS) return false;
    u.imported = false;
    u.generation = f.generation;
    return true;
}

void Presenter::waitSlot(int slot) {
    if (slot < 0 || slot >= X11Capture::kSlots) return;
    Upload& u = uploads_[slot];
    if (u.imported && u.lastUse) vkWaitForFences(dev_, 1, &u.lastUse, VK_TRUE, UINT64_MAX);
}

// ----------------------------------------------------------- synthesis ----

int Presenter::synthesize(const X11Capture::Frame& f) {
    if (!swapchain_ || f.slot < 0 || f.slot >= X11Capture::kSlots) return 1;
    const size_t bytes = static_cast<size_t>(f.stride) * f.height;
    Upload& u = uploads_[f.slot];
    if (u.generation != f.generation || !u.buffer.buffer ||
        (u.imported && u.buffer.size < bytes) || (!u.imported && u.buffer.size < bytes)) {
        if (!prepareUpload(u, f, bytes)) return -1;
    }
    if (!fg_ || f.width != captureW_ || f.height != captureH_) {
        if (!rebuildEngine(f.width, f.height)) return -1;
    }
    if (!u.imported) {
        if (u.lastUse) vkWaitForFences(dev_, 1, &u.lastUse, VK_TRUE, UINT64_MAX);
        std::memcpy(u.mapped, f.data, bytes);
    }

    const auto now = clock::now();
    const auto content = f.damagedAt.time_since_epoch().count() != 0 ? f.damagedAt : now;
    double dt = 0.0;
    if (lastArrival_.time_since_epoch().count() != 0) {
        dt = std::chrono::duration<double, std::milli>(content - lastArrival_).count();
    }
    intervalMs_ = intervalFilter_.update(intervalMs_, dt);
    lastArrival_ = content;

    const Config& cfg = layerDev_.config;
    int mult = genEnabled_ ? std::max(1, cfg.multiplier) : 1;
    double periodMs = 0.0;
    const double target = cfg.targetFps < 0.f || cfg.targetFps == 0.f ? displayHz_ : double(cfg.targetFps);
    if (genEnabled_ && cfg.adaptive && target > 0.0 && mult > 1) {
        periodMs = 1000.0 / target;
        adaptiveAcc_ += intervalMs_ / periodMs;
        const int n = std::clamp(static_cast<int>(adaptiveAcc_), 1, mult);
        adaptiveAcc_ = std::clamp(adaptiveAcc_ - n, 0.0, 0.999);
        mult = n;
        if (n < 2) periodMs = 0.0;
    }
    const bool interpolate = prev_ >= 0 && mult > 1;
    const uint32_t cur = cur_;
    const int prev = prev_;
    fg_->setHud(hudGame_, interpolate ? hudOut_ : hudGame_, 0, 0);

    CmdSlot& s = nextCmd();
    VkCommandBuffer cb = s.cmd;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    // The captured frame into the history (overwritten whole).
    VkBufferMemoryBarrier hostRead{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    hostRead.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    hostRead.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    hostRead.srcQueueFamilyIndex = hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostRead.buffer = u.buffer.buffer;
    hostRead.size = VK_WHOLE_SIZE;
    VkImageMemoryBarrier pre = imageBarrier(history_[cur].image, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &hostRead, 1, &pre);
    VkBufferImageCopy region{};
    region.bufferRowLength = f.stride / 4;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {captureW_, captureH_, 1};
    vkCmdCopyBufferToImage(cb, u.buffer.buffer, history_[cur].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    VkImageMemoryBarrier post = imageBarrier(history_[cur].image, VK_ACCESS_TRANSFER_WRITE_BIT,
                                             VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &post);

    fg_->profileBegin(cb, static_cast<uint32_t>(&s - cmds_));
    fg_->recordAcquireSources(cb, prev, static_cast<int>(cur));
    fg_->recordAnalysis(cb, parity_, cur);
    FrameGen::FrameTiming timing;
    timing.sinceLastMs = dt;
    timing.intervalMs = intervalMs_;
    timing.multiplier = interpolate ? mult : 1;
    timing.reset = prev < 0 || !(dt > 0.5 && dt < 500.0);
    if (interpolate && periodMs > 0.0) {
        const FrameOrder order = FrameOrder::of(cfg, mult);
        timing.haveDisplay = true;
        timing.displayMs[7] = float(1.0 + periodMs * order.real);
        for (int o = 0; o + 1 < mult && o < 7; ++o) timing.displayMs[o] = float(1.0 + periodMs * order.position(o));
    }
    fg_->recordFrameState(cb, timing);
    if (interpolate) {
        fg_->recordFlow(cb, parity_);
        const FrameOrder order = FrameOrder::of(cfg, mult);
        for (int o = 0; o + 1 < mult; ++o)
            fg_->recordInterpolate(cb, static_cast<uint32_t>(prev), cur, order.t(o), static_cast<uint32_t>(o));
    }
    if (fg_->packReal()) fg_->recordUpscaleReal(cb, cur, interpolate ? prev : -1);
    fg_->recordReleaseSources(cb, prev, static_cast<int>(cur));
    vkEndCommandBuffer(cb);

    VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &cb;
    vkResetFences(dev_, 1, &s.fence);
    const VkResult sr = vkQueueSubmit(queue_, 1, &sub, s.fence);
    if (sr != VK_SUCCESS) {
        BDEX_ERR("synthesis submit failed (%s)", vkResultName(sr));
        return -1;
    }
    lastSynthFence_ = s.fence;
    u.lastUse = s.fence;

    job_.frames = interpolate ? mult : 1;
    job_.realIndex = interpolate ? FrameOrder::of(cfg, mult).real : 0;
    job_.start = now;
    job_.intervalMs = interpolate && periodMs > 0.0 ? periodMs * mult : intervalMs_;
    prev_ = static_cast<int>(cur);
    cur_ = (cur + 1) % kHistory;
    parity_ ^= 1;
    return 0;
}

// ------------------------------------------------------------- present ----

int Presenter::present(int index) {
    if (!swapchain_ || !fg_) return 1;
    if (index < 0 || index >= job_.frames) return 0;
    const bool real = index == job_.realIndex;
    const uint32_t output = static_cast<uint32_t>(index < job_.realIndex ? index : index - 1);
    const uint32_t source = static_cast<uint32_t>(prev_);
    CmdSlot& s = nextCmd();
    uint32_t img = 0;
    const VkResult ar = vkAcquireNextImageKHR(dev_, swapchain_, UINT64_MAX, s.acquireSem, VK_NULL_HANDLE, &img);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) return 1;
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
        BDEX_ERR("vkAcquireNextImageKHR failed (%s)", vkResultName(ar));
        return -1;
    }
    VkCommandBuffer cb = s.cmd;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    if (real) fg_->recordCopySource(cb, source, scImages_[img]);
    else fg_->recordCopyOutput(cb, output, scImages_[img]);
    vkEndCommandBuffer(cb);
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    sub.waitSemaphoreCount = 1;
    sub.pWaitSemaphores = &s.acquireSem;
    sub.pWaitDstStageMask = &waitStage;
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &cb;
    sub.signalSemaphoreCount = 1;
    sub.pSignalSemaphores = &semDone_[img];
    vkResetFences(dev_, 1, &s.fence);
    const VkResult sr = vkQueueSubmit(queue_, 1, &sub, s.fence);
    if (sr != VK_SUCCESS) {
        BDEX_ERR("present submit failed (%s)", vkResultName(sr));
        return -1;
    }
    lastCopyFence_ = s.fence;
    VkSwapchainKHR sw = swapchain_;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &semDone_[img];
    pi.swapchainCount = 1;
    pi.pSwapchains = &sw;
    pi.pImageIndices = &img;
    VkResult pr = vkQueuePresentKHR(queue_, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR) return 1;
    if (pr == VK_SUBOPTIMAL_KHR) {
        suboptimal_ = true;
        pr = VK_SUCCESS;
    }
    if (pr != VK_SUCCESS) {
        BDEX_ERR("vkQueuePresentKHR failed (%s)", vkResultName(pr));
        return -1;
    }
    return 0;
}

Presenter::clock::time_point Presenter::waitFence(VkFence fence, bool (*cancel)(void*), void* ctx) {
    if (!fence) return clock::now();
    const auto limit = clock::now() + std::chrono::milliseconds(250);
    for (;;) {
        if (vkWaitForFences(dev_, 1, &fence, VK_TRUE, 500'000) != VK_TIMEOUT) break;
        if ((cancel && cancel(ctx)) || clock::now() >= limit) break;
    }
    return clock::now();
}

Presenter::clock::time_point Presenter::waitSynthesis() { return waitFence(lastSynthFence_, nullptr, nullptr); }
Presenter::clock::time_point Presenter::waitLastCopy(bool (*cancel)(void*), void* ctx) {
    return waitFence(lastCopyFence_, cancel, ctx);
}

// -------------------------------------------------------------- status ----

void Presenter::publishStatus(const std::string& target, double captureFps, double outFps, double latencyMs) {
    StatusEntry e;
    e.kind = "capture";
    e.target = target;
    e.state = genEnabled_ ? "generating" : "paused";
    e.mode = layerDev_.config.modeName();
    e.multiplier = genEnabled_ ? std::max(1, layerDev_.config.multiplier) : 1;
    e.gameFps = captureFps;
    e.outFps = outFps;
    e.latencyMs = latencyMs;
    e.refreshHz = displayHz_;
    e.width = width_;
    e.height = height_;
    e.device = gpuName_;
    if (fg_) e.passes = fg_->passNames();
    {
        std::lock_guard<std::mutex> lock(layerDev_.onlineTuneMutex);
        if (layerDev_.onlineTune) e.tune = layerDev_.onlineTune->status();
    }
    statusSet(statusKey(this), e);
}

void Presenter::shutdown() {
    statusRemove(statusKey(this));
    if (!dev_) {
        if (surface_) vkDestroySurfaceKHR(inst_, surface_, nullptr);
        if (inst_) vkDestroyInstance(inst_, nullptr);
        surface_ = VK_NULL_HANDLE;
        inst_ = VK_NULL_HANDLE;
        return;
    }
    vkDeviceWaitIdle(dev_);
    destroyEngine();
    captureW_ = captureH_ = 0;
    for (auto& u : uploads_) destroyUpload(u);
    destroySwapchain();
    for (auto& s : cmds_) {
        if (s.acquireSem) vkDestroySemaphore(dev_, s.acquireSem, nullptr);
        if (s.fence) vkDestroyFence(dev_, s.fence, nullptr);
        s = CmdSlot{};
    }
    if (cpool_) vkDestroyCommandPool(dev_, cpool_, nullptr);
    cpool_ = VK_NULL_HANDLE;
    layerDev_.destroyPipelineCache();
    vkDestroyDevice(dev_, nullptr);
    dev_ = VK_NULL_HANDLE;
    if (surface_) vkDestroySurfaceKHR(inst_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    vkDestroyInstance(inst_, nullptr);
    inst_ = VK_NULL_HANDLE;
    phys_ = VK_NULL_HANDLE;
}

} // namespace bdex
