#include "vkctx.h"
#include "log.h"

#include "framegen.h"
#include "vk_util.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <vector>

namespace bdex {

// vk_util.h's VK_CHECK throws (the layer's convention); here init paths
// return false with a log line instead.
#define VKC(x)                                                             \
    do {                                                                   \
        VkResult r_ = (x);                                                 \
        if (r_ != VK_SUCCESS) {                                            \
            LOGE("vulkan error %s at %s:%d", vkResultName(r_), __FILE__,   \
                 __LINE__);                                                \
            return false;                                                  \
        }                                                                  \
    } while (0)

static bool hasExt(const char* name, const std::vector<VkExtensionProperties>& list) {
    for (auto& e : list)
        if (std::strcmp(name, e.extensionName) == 0) return true;
    return false;
}

// Vulkan 1.1 core covers the rest (external memory capabilities, dedicated
// allocation, get_physical_device_properties2).
static const char* kDevExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                                 VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
                                 VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
                                 VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME};

VkCtx::~VkCtx() { shutdown(); }

// ---------------------------------------------------------------- init ----

bool VkCtx::init(HWND overlay, int gpuIndex, bool preferFifo, const Config& cfg) {
    preferFifo_ = preferFifo;

    uint32_t loaderVersion = VK_API_VERSION_1_0;
    vkEnumerateInstanceVersion(&loaderVersion);
    if (loaderVersion < VK_API_VERSION_1_1) {
        LOGE("Vulkan 1.1 instance required (loader reports %u.%u)",
             VK_VERSION_MAJOR(loaderVersion), VK_VERSION_MINOR(loaderVersion));
        return false;
    }
    uint32_t nInst = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &nInst, nullptr);
    std::vector<VkExtensionProperties> instExts(nInst);
    vkEnumerateInstanceExtensionProperties(nullptr, &nInst, instExts.data());
    std::vector<const char*> wantInst = {VK_KHR_SURFACE_EXTENSION_NAME,
                                         VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    for (auto e : wantInst)
        if (!hasExt(e, instExts)) {
            LOGE("instance extension %s missing", e);
            return false;
        }
    std::vector<const char*> layers;
    if (std::getenv("BDEX_CAP_VALIDATION")) layers.push_back("VK_LAYER_KHRONOS_validation");

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "bdex_capture";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<uint32_t>(wantInst.size());
    ici.ppEnabledExtensionNames = wantInst.data();
    ici.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    VKC(vkCreateInstance(&ici, nullptr, &inst_));

    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = GetModuleHandleW(nullptr);
    sci.hwnd = overlay;
    VKC(vkCreateWin32SurfaceKHR(inst_, &sci, nullptr, &surface_));

    // GPU: needs GRAPHICS+COMPUTE with present, the external-memory/keyed-mutex
    // extension set, and D3D11-texture import for B8G8R8A8_UNORM SAMPLED /
    // optimal. Among the usable ones, a discrete GPU wins over an integrated
    // one (laptops: the game renders on the dGPU, so should we), unless
    // --gpu picks one explicitly.
    struct Candidate {
        VkPhysicalDevice pd;
        uint32_t qf;
        VkPhysicalDeviceProperties props;
        bool dedicatedOnly;
    };
    std::vector<Candidate> usable;
    uint32_t nPhys = 0;
    vkEnumeratePhysicalDevices(inst_, &nPhys, nullptr);
    std::vector<VkPhysicalDevice> physList(nPhys);
    vkEnumeratePhysicalDevices(inst_, &nPhys, physList.data());
    for (VkPhysicalDevice pd : physList) {
        VkPhysicalDeviceProperties gp;
        vkGetPhysicalDeviceProperties(pd, &gp);
        if (gp.apiVersion < VK_API_VERSION_1_1) {
            LOGW("%s: Vulkan 1.0 only, skipped", gp.deviceName);
            continue;
        }
        uint32_t nExt = 0;
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &nExt, nullptr);
        std::vector<VkExtensionProperties> exts(nExt);
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &nExt, exts.data());
        bool ok = true;
        for (auto e : kDevExts)
            if (!hasExt(e, exts)) {
                LOGW("%s: missing %s, skipped", gp.deviceName, e);
                ok = false;
            }
        if (!ok) continue;

        uint32_t nQ = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nQ, nullptr);
        std::vector<VkQueueFamilyProperties> qps(nQ);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nQ, qps.data());
        uint32_t qf = UINT32_MAX;
        for (uint32_t i = 0; i < nQ; ++i) {
            VkBool32 surf = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface_, &surf);
            if (surf && (qps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                (qps[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                qf = i;
                break;
            }
        }
        if (qf == UINT32_MAX) {
            LOGW("%s: no GRAPHICS+COMPUTE queue that can present, skipped", gp.deviceName);
            continue;
        }

        VkPhysicalDeviceExternalImageFormatInfo extIn{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        extIn.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        VkPhysicalDeviceImageFormatInfo2 fmtIn{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &extIn};
        fmtIn.format = VK_FORMAT_B8G8R8A8_UNORM;
        fmtIn.type = VK_IMAGE_TYPE_2D;
        fmtIn.tiling = VK_IMAGE_TILING_OPTIMAL;
        fmtIn.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        VkExternalImageFormatProperties extOut{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 fmtOut{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &extOut};
        if (vkGetPhysicalDeviceImageFormatProperties2(pd, &fmtIn, &fmtOut) != VK_SUCCESS ||
            !(extOut.externalMemoryProperties.externalMemoryFeatures &
              VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
            LOGW("%s: cannot import D3D11 B8G8R8A8 textures, skipped", gp.deviceName);
            continue;
        }
        usable.push_back({pd, qf, gp,
                          (extOut.externalMemoryProperties.externalMemoryFeatures &
                           VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0});
    }
    if (usable.empty()) {
        LOGE("no GPU with D3D11 import + keyed mutex + GRAPHICS/COMPUTE queue");
        return false;
    }
    static const char* kKind[] = {"other", "integrated", "discrete", "virtual", "software"};
    auto kind = [](const VkPhysicalDeviceProperties& p) {
        return kKind[p.deviceType <= 4 ? p.deviceType : 0];
    };
    for (size_t i = 0; i < usable.size(); ++i)
        LOGI("gpu %zu: %s (%s)", i, usable[i].props.deviceName, kind(usable[i].props));
    size_t pick = 0;
    if (gpuIndex >= 0) {
        if (static_cast<size_t>(gpuIndex) >= usable.size()) {
            LOGE("--gpu %d: only %zu usable GPU(s)", gpuIndex, usable.size());
            return false;
        }
        pick = static_cast<size_t>(gpuIndex);
    } else {
        auto score = [](VkPhysicalDeviceType t) {
            return t == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                   : t == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                   : t == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU    ? 1
                                                                 : 0;
        };
        for (size_t i = 1; i < usable.size(); ++i)
            if (score(usable[i].props.deviceType) > score(usable[pick].props.deviceType)) pick = i;
    }
    const Candidate& c = usable[pick];
    phys_ = c.pd;
    qf_ = c.qf;
    LOGI("using gpu %zu: %s (%s, queue family %u, D3D11 import%s)", pick, c.props.deviceName,
         kind(c.props), c.qf, c.dedicatedOnly ? " dedicated-only" : "");

    VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp};
    vkGetPhysicalDeviceProperties2(phys_, &p2);
    luidValid_ = idp.deviceLUIDValid == VK_TRUE;
    if (luidValid_) std::memcpy(&luid_, idp.deviceLUID, sizeof luid_);
    else LOGW("driver reports no LUID: the D3D11 device will use the default adapter");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo dq{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    dq.queueFamilyIndex = qf_;
    dq.queueCount = 1;
    dq.pQueuePriorities = &prio;
    VkPhysicalDeviceFeatures feats{};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &dq;
    dci.enabledExtensionCount = static_cast<uint32_t>(std::size(kDevExts));
    dci.ppEnabledExtensionNames = kDevExts;
    dci.pEnabledFeatures = &feats;
    VKC(vkCreateDevice(phys_, &dci, nullptr, &dev_));
    vkGetDeviceQueue(dev_, qf_, 0, &queue_);

    getMemProps_ = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
        vkGetDeviceProcAddr(dev_, "vkGetMemoryWin32HandlePropertiesKHR"));
    if (!getMemProps_) {
        LOGE("vkGetMemoryWin32HandlePropertiesKHR missing (old driver?)");
        return false;
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

    // The layer's DeviceData over our plain device: FrameGen only needs the
    // dispatch table, the properties, the config and the resource helpers.
    layerInst_.vt.load(inst_, vkGetInstanceProcAddr);
    layerInst_.instance = inst_;
    layerInst_.apiVersion = VK_API_VERSION_1_1;
    layerInst_.config = cfg;
    layerDev_.vt.load(dev_, vkGetDeviceProcAddr);
    layerDev_.inst = &layerInst_;
    layerDev_.physDev = phys_;
    layerDev_.device = dev_;
    layerDev_.config = cfg;
    layerDev_.props = c.props;
    vkGetPhysicalDeviceMemoryProperties(phys_, &layerDev_.memProps);
    layerDev_.queue = queue_;
    layerDev_.queueFamily = qf_;
    layerDev_.queueIndex = 0;
    layerDev_.layerUsable = true;
    layerDev_.cmdPool = cpool_;
    layerDev_.config.autoConfigure(layerDev_.props, layerDev_.memProps);
    const Config& fc = layerDev_.config;
    LOGI("frame generation: x%d %s, preset %s, upscale filter %d, sharpness %.2f, hud %d",
         fc.multiplier, fc.extrapolate ? "extrapolate" : "interpolate",
         fc.autoTunedTo.empty() ? "(explicit)" : fc.autoTunedTo.c_str(), fc.upscaleFilter,
         static_cast<double>(fc.sharpness), fc.overlayMode);
    LOGI("vulkan ready");
    return true;
}

bool VkCtx::luid(LUID& out) const {
    if (!luidValid_) return false;
    out = luid_;
    return true;
}

// -------------------------------------------------------------- sizing ----

const char* VkCtx::presentModeName() const {
    switch (presentMode_) {
    case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo-relaxed";
    default: return "fifo";
    }
}

bool VkCtx::fifo() const {
    return presentMode_ == VK_PRESENT_MODE_FIFO_KHR || presentMode_ == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
}

bool VkCtx::createSwapchain(uint32_t w, uint32_t h) {
    VkSurfaceCapabilitiesKHR caps;
    VKC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps));
    // On Win32 currentExtent follows the window; a 0 extent means minimised.
    if (caps.currentExtent.width != UINT32_MAX) {
        w = caps.currentExtent.width;
        h = caps.currentExtent.height;
    }
    if (w == 0 || h == 0) return false;
    width_ = std::clamp(w, caps.minImageExtent.width, caps.maxImageExtent.width);
    height_ = std::clamp(h, caps.minImageExtent.height, caps.maxImageExtent.height);

    // Mailbox lets our pacing space the frames; immediate does too (with
    // tearing); FIFO leaves the pacing to the display (see present()).
    uint32_t nModes = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &nModes, nullptr);
    std::vector<VkPresentModeKHR> modes(nModes);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &nModes, modes.data());
    auto have = [&](VkPresentModeKHR m) {
        return std::find(modes.begin(), modes.end(), m) != modes.end();
    };
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
        if (f.format == scFormat_ && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            found = true;
    if (!found) {
        LOGE("B8G8R8A8_UNORM/SRGB surface format not supported");
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
    sw.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
        sw.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
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
    LOGI("swapchain %ux%u, %u images, %s", width_, height_, nImg, presentModeName());

    // The engine's display extent changed: rebuild it (the history is lost,
    // the next frame is analysed without a predecessor).
    if (captureW_ && !rebuildEngine(captureW_, captureH_)) return false;
    return true;
}

void VkCtx::destroySwapchain() {
    vkDeviceWaitIdle(dev_);
    for (auto s : semDone_) vkDestroySemaphore(dev_, s, nullptr);
    semDone_.clear();
    if (swapchain_) vkDestroySwapchainKHR(dev_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    scImages_.clear();
    width_ = height_ = 0;
}

bool VkCtx::setSize(uint32_t w, uint32_t h) {
    if (!dev_) return false;
    destroySwapchain();
    if (!createSwapchain(w, h)) {
        destroySwapchain();  // partial state from a failed create
        return false;
    }
    return true;
}

// -------------------------------------------------------------- engine ----

void VkCtx::destroyEngine() {
    vkDeviceWaitIdle(dev_);
    fg_.reset();
    for (auto& h : history_)
        if (h.image) layerDev_.destroyImage(h);
    prev_ = -1;
    parity_ = 0;
    generated_ = false;
}

bool VkCtx::rebuildEngine(uint32_t cw, uint32_t ch) {
    destroyEngine();
    captureW_ = cw;
    captureH_ = ch;
    try {
        // History images play the role of the game's swapchain images in the
        // layer: analysed in place, copied to the display when they are the
        // real frame (sampled when upscaling / drawing the HUD).
        std::vector<VkImage> sources;
        for (auto& h : history_) {
            h = layerDev_.createImage(scFormat_, {cw, ch},
                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                          VK_IMAGE_USAGE_SAMPLED_BIT,
                                      false);
            sources.push_back(h.image);
        }
        const uint32_t outputs = static_cast<uint32_t>(std::max(1, layerDev_.config.multiplier) - 1);
        fg_ = std::make_unique<FrameGen>(layerDev_, scFormat_, VkExtent2D{cw, ch},
                                         VkExtent2D{width_, height_}, outputs, sources);
    } catch (const std::exception& e) {
        LOGE("frame generation setup failed: %s", e.what());
        destroyEngine();
        return false;
    }
    LOGI("engine: capture %ux%u -> display %ux%u, %d frame(s) per capture", cw, ch, width_,
         height_, std::max(1, layerDev_.config.multiplier));
    return true;
}

VkCtx::CmdSlot& VkCtx::nextCmd() {
    CmdSlot& s = cmds_[cmdCursor_];
    cmdCursor_ = (cmdCursor_ + 1) % kCmdSlots;
    vkWaitForFences(dev_, 1, &s.fence, VK_TRUE, UINT64_MAX);
    return s;  // the fence is reset right before the submit that uses it
}

void VkCtx::setHud(int gameFps, int outFps) {
    hudGame_ = gameFps;
    hudOut_ = outFps;
}

// -------------------------------------------------------------- import ----

void VkCtx::destroyImported(SlotVk& sl) {
    vkDeviceWaitIdle(dev_);
    if (sl.image) vkDestroyImage(dev_, sl.image, nullptr);
    if (sl.mem) vkFreeMemory(dev_, sl.mem, nullptr);  // drops our reference to
                                                      // the D3D11 payload
    sl = SlotVk{};
}

bool VkCtx::importSlot(SlotVk& sl, const Capture::Frame& f) {
    VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext};
    ic.imageType = VK_IMAGE_TYPE_2D;
    ic.format = VK_FORMAT_B8G8R8A8_UNORM;
    ic.extent = {f.width, f.height, 1};
    ic.mipLevels = 1;
    ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKC(vkCreateImage(dev_, &ic, nullptr, &sl.image));

    VkMemoryRequirements reqs;
    vkGetImageMemoryRequirements(dev_, sl.image, &reqs);
    VkMemoryWin32HandlePropertiesKHR props{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
    VKC(getMemProps_(dev_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, f.handle, &props));
    uint32_t bits = props.memoryTypeBits & reqs.memoryTypeBits;
    if (bits == 0) bits = props.memoryTypeBits;  // trust the handle over the image
    const VkPhysicalDeviceMemoryProperties& mp = layerDev_.memProps;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if (!(bits & (1u << i))) continue;
        if (type == UINT32_MAX) type = i;
        if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            type = i;
            break;
        }
    }
    if (type == UINT32_MAX) {
        LOGE("no memory type for the imported texture");
        return false;
    }
    // D3D11 textures import as dedicated allocations (the handle *is* the
    // texture); allocationSize is ignored for this handle type. The NT handle
    // stays owned by Capture — the import only references the payload.
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = sl.image;
    VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
                                         &ded};
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    imp.handle = f.handle;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp};
    ai.allocationSize = reqs.size;
    ai.memoryTypeIndex = type;
    VKC(vkAllocateMemory(dev_, &ai, nullptr, &sl.mem));
    VKC(vkBindImageMemory(dev_, sl.image, sl.mem, 0));
    sl.generation = f.generation;
    LOGI("imported ring slot %u (%ux%u, generation %u)", f.slot, f.width, f.height,
         f.generation);
    return true;
}

// ----------------------------------------------------------- synthesis ----

int VkCtx::synthesize(const Capture::Frame& f, bool& consumed) {
    consumed = false;
    if (!swapchain_ || f.slot >= Capture::kRingSize) return 1;
    SlotVk& sl = slots_[f.slot];
    if (sl.generation != f.generation) {
        // Ring recreated: free the import of the previous ring (the keyed
        // mutex has long been released by then), let the capture side drop
        // that ring's textures once no slot references it, then import.
        uint32_t oldGen = sl.generation;
        if (sl.image) destroyImported(sl);
        if (oldGen != UINT32_MAX && cap_) {
            bool others = false;
            for (auto& o : slots_) others = others || (o.generation == oldGen);
            if (!others) cap_->releaseGraveyard(oldGen);
        }
        if (!importSlot(sl, f)) return -1;
    }
    if (!fg_ || f.width != captureW_ || f.height != captureH_) {
        if (!rebuildEngine(f.width, f.height)) return -1;
    }

    // Capture interval estimate (EMA) for the pacing and the HUD.
    const auto now = clock::now();
    if (lastArrival_.time_since_epoch().count() != 0) {
        double dt = std::chrono::duration<double, std::milli>(now - lastArrival_).count();
        if (dt > 0.5 && dt < 500.0) intervalMs_ = intervalMs_ * 0.8 + dt * 0.2;
    }
    lastArrival_ = now;

    const Config& cfg = layerDev_.config;
    const int mult = std::max(1, cfg.multiplier);
    const bool interpolate = prev_ >= 0 && mult > 1;
    const uint32_t cur = cur_;
    const int prev = prev_;
    fg_->setHud(hudGame_, interpolate ? hudOut_ : hudGame_, 0, 0);

    CmdSlot& s = nextCmd();
    VkCommandBuffer cb = s.cmd;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);

    // Imported texture: take ownership from the external (D3D11) side; the
    // keyed mutex on the submit orders the D3D11 copy before us. History
    // image: overwritten whole (the previous content is a frame already
    // presented, its last reader precedes us in queue order).
    VkImageMemoryBarrier pre[2];
    pre[0] = imageBarrier(sl.image, 0, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                          VK_IMAGE_LAYOUT_GENERAL);
    pre[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    pre[0].dstQueueFamilyIndex = qf_;
    pre[1] = imageBarrier(history_[cur].image, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 2, pre);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {captureW_, captureH_, 1};
    vkCmdCopyImage(cb, sl.image, VK_IMAGE_LAYOUT_GENERAL, history_[cur].image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    // Back to D3D11; the history image goes to PRESENT_SRC, the layout
    // FrameGen expects its sources in between uses.
    VkImageMemoryBarrier post[2];
    post[0] = imageBarrier(sl.image, VK_ACCESS_TRANSFER_READ_BIT, 0, VK_IMAGE_LAYOUT_GENERAL,
                           VK_IMAGE_LAYOUT_GENERAL);
    post[0].srcQueueFamilyIndex = qf_;
    post[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    post[1] = imageBarrier(history_[cur].image, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         0, nullptr, 0, nullptr, 2, post);

    // Same sequence as VirtualSwapchain::present: analyse the new frame,
    // flow against the previous one, synthesise the in-between (or
    // predicted) frames, resample the real frame if upscaling / HUD.
    fg_->recordAcquireSources(cb, prev, static_cast<int>(cur));
    fg_->recordAnalysis(cb, parity_, cur);
    if (interpolate) {
        fg_->recordFlow(cb, parity_);
        const float base = cfg.extrapolate ? 1.f : 0.f;
        for (int i = 1; i < mult; ++i)
            fg_->recordInterpolate(cb, static_cast<uint32_t>(prev), cur,
                                   base + static_cast<float>(i) / static_cast<float>(mult),
                                   static_cast<uint32_t>(i - 1));
    }
    if (fg_->packReal()) fg_->recordUpscaleReal(cb, cur);
    fg_->recordReleaseSources(cb, prev, static_cast<int>(cur));
    vkEndCommandBuffer(cb);

    // Keyed mutex on the submit: wait for the D3D11 side's ReleaseSync(1)
    // before the command buffer runs, hand the texture back with key 0 after.
    const uint64_t acquireKey = 1, releaseKey = 0;
    const uint32_t acquireTimeoutMs = 1000;
    VkWin32KeyedMutexAcquireReleaseInfoKHR km{
        VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR};
    km.acquireCount = 1;
    km.pAcquireSyncs = &sl.mem;
    km.pAcquireKeys = &acquireKey;
    km.pAcquireTimeouts = &acquireTimeoutMs;
    km.releaseCount = 1;
    km.pReleaseSyncs = &sl.mem;
    km.pReleaseKeys = &releaseKey;
    VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO, &km};
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &cb;
    vkResetFences(dev_, 1, &s.fence);
    VkResult sr = vkQueueSubmit(queue_, 1, &sub, s.fence);
    if (sr != VK_SUCCESS) {
        LOGE("synthesis submit failed (%s)", vkResultName(sr));
        return -1;
    }
    consumed = true;

    job_.frames = interpolate ? mult : 1;
    job_.realFirst = cfg.extrapolate;
    job_.start = now;
    job_.intervalMs = intervalMs_;
    generated_ = interpolate;
    prev_ = static_cast<int>(cur);
    cur_ = (cur + 1) % kHistory;
    parity_ ^= 1;
    ++frameNo_;
    return 0;
}

// ------------------------------------------------------------- present ----

int VkCtx::present(int index) {
    if (!swapchain_ || !fg_) return 1;
    const Job& job = job_;
    if (index < 0 || index >= job.frames) return 0;
    const bool real = job.realFirst ? index == 0 : index == job.frames - 1;
    const uint32_t output = job.realFirst ? static_cast<uint32_t>(index - 1)
                                          : static_cast<uint32_t>(index);
    const uint32_t source = static_cast<uint32_t>(prev_);  // the frame synthesize() just took

    CmdSlot& s = nextCmd();
    uint32_t img = 0;
    VkResult ar = vkAcquireNextImageKHR(dev_, swapchain_, UINT64_MAX, s.acquireSem, VK_NULL_HANDLE,
                                        &img);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) return 1;
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
        LOGE("vkAcquireNextImageKHR failed (%s)", vkResultName(ar));
        return -1;
    }

    VkCommandBuffer cb = s.cmd;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    if (real) fg_->recordCopySource(cb, source, scImages_[img]);
    else fg_->recordCopyOutput(cb, output, scImages_[img]);
    vkEndCommandBuffer(cb);

    // The copy reads images written by the synthesis submission, which
    // precedes it on the same queue: queue order is the only dependency.
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
    VkResult sr = vkQueueSubmit(queue_, 1, &sub, s.fence);
    if (sr != VK_SUCCESS) {
        LOGE("present submit failed (%s)", vkResultName(sr));
        return -1;
    }

    VkSwapchainKHR sw = swapchain_;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &semDone_[img];
    pi.swapchainCount = 1;
    pi.pSwapchains = &sw;
    pi.pImageIndices = &img;
    VkResult pr = vkQueuePresentKHR(queue_, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) return 1;
    if (pr != VK_SUCCESS) {
        LOGE("vkQueuePresentKHR failed (%s)", vkResultName(pr));
        return -1;
    }
    return 0;
}

// ------------------------------------------------------------- shutdown ---

void VkCtx::shutdown() {
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
    for (auto& sl : slots_) {
        if (sl.image) vkDestroyImage(dev_, sl.image, nullptr);
        if (sl.mem) vkFreeMemory(dev_, sl.mem, nullptr);
        sl = SlotVk{};
    }
    destroySwapchain();
    for (auto& s : cmds_) {
        if (s.acquireSem) vkDestroySemaphore(dev_, s.acquireSem, nullptr);
        if (s.fence) vkDestroyFence(dev_, s.fence, nullptr);
        s = CmdSlot{};
    }
    if (cpool_) vkDestroyCommandPool(dev_, cpool_, nullptr);
    cpool_ = VK_NULL_HANDLE;
    vkDestroyDevice(dev_, nullptr);
    dev_ = VK_NULL_HANDLE;
    if (surface_) vkDestroySurfaceKHR(inst_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    vkDestroyInstance(inst_, nullptr);
    inst_ = VK_NULL_HANDLE;
    phys_ = VK_NULL_HANDLE;
    LOGI("vulkan shutdown");
}

} // namespace bdex
