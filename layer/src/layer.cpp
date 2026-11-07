// Vulkan layer entry points: instance/device lifecycle, dispatch and the
// swapchain hooks that route through VirtualSwapchain.
#include "config.h"
#include "crash.h"
#include "status.h"
#include "device.h"
#include "dispatch.h"
#include "gamescope.h"
#include "log.h"
#include "platform.h"
#include "present_slice.h"
#include "swapchain.h"
#include "vk_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef BDEX_LAYER_NAME
#define BDEX_LAYER_NAME "VK_LAYER_BDEX_framegen"
#endif

namespace bdex {

namespace {

std::mutex g_mapMutex;
// Deliberately never destroyed: many games exit without destroying their
// device or swapchains, and tearing these down from static destructors would
// call into the driver after the window system is gone (a segfault at exit
// under native Wayland, in wl_proxy_marshal from vkDestroySwapchainKHR). The
// process is ending anyway; the OS reclaims everything.
std::unordered_map<void*, std::unique_ptr<InstanceData>>& g_instances =
    *new std::unordered_map<void*, std::unique_ptr<InstanceData>>();
std::unordered_map<void*, std::unique_ptr<DeviceData>>& g_devices =
    *new std::unordered_map<void*, std::unique_ptr<DeviceData>>();
std::once_flag g_logInit;

const VkLayerProperties g_layerProps = {
    BDEX_LAYER_NAME,
    VK_MAKE_API_VERSION(0, 1, 4, 0),
    1,
    "BDEX frame generation layer",
};

void initLogging() {
    std::call_once(g_logInit, [] {
        Config c = Config::load();
        log_set_level(c.logLevel);
        if (!c.logFile.empty()) log_set_file(c.logFile.c_str());
        BDEX_INFO("%s v%s loaded (%s)", BDEX_LAYER_NAME, BDEX_LAYER_VERSION_STR, c.describe().c_str());
    });
}

} // namespace

InstanceData* getInstance(void* key) {
    std::lock_guard<std::mutex> lock(g_mapMutex);
    auto it = g_instances.find(key);
    return it == g_instances.end() ? nullptr : it->second.get();
}

DeviceData* getDevice(void* key) {
    std::lock_guard<std::mutex> lock(g_mapMutex);
    auto it = g_devices.find(key);
    return it == g_devices.end() ? nullptr : it->second.get();
}

namespace {

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator,
                                              VkInstance* pInstance) {
    initLogging();
    VkLayerInstanceCreateInfo* chain = const_cast<VkLayerInstanceCreateInfo*>(
        findChain<VkLayerInstanceCreateInfo>(pCreateInfo->pNext, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO));
    while (chain && chain->function != VK_LAYER_LINK_INFO)
        chain = const_cast<VkLayerInstanceCreateInfo*>(
            findChain<VkLayerInstanceCreateInfo>(chain->pNext, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO));
    if (!chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gpa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;  // advance for the next layer

    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(gpa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!createInstance) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = createInstance(pCreateInfo, pAllocator, pInstance);
    if (r != VK_SUCCESS) return r;

    auto data = std::make_unique<InstanceData>();
    data->instance = *pInstance;
    data->vt.load(*pInstance, gpa);
    data->config = Config::load();
    if (pCreateInfo->pApplicationInfo && pCreateInfo->pApplicationInfo->apiVersion)
        data->apiVersion = pCreateInfo->pApplicationInfo->apiVersion;
    if (pCreateInfo->pApplicationInfo && pCreateInfo->pApplicationInfo->pEngineName)
        data->engine = pCreateInfo->pApplicationInfo->pEngineName;
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i)
        data->surfaceCaps2 |= strcmp(pCreateInfo->ppEnabledExtensionNames[i], "VK_KHR_get_surface_capabilities2") == 0;
    {
        std::string eng = data->engine;
        for (char& c : eng) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        data->dxvk = eng.find("dxvk") != std::string::npos;
        const bool translation = data->dxvk || eng.find("vkd3d") != std::string::npos;
        data->presentWaitOffered = translation && data->config.enabled && data->config.hidePresentExt && data->config.fpsLimitWait;
        data->fifoOnly.store(data->presentWaitOffered && data->config.fpsLimit != 0.f && data->config.capsThisGame(),
                             std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> lock(g_mapMutex);
        g_instances[dispatchKey(*pInstance)] = std::move(data);
    }
    BDEX_DBG("instance created (api %u.%u)", VK_API_VERSION_MAJOR(pCreateInfo->pApplicationInfo ? pCreateInfo->pApplicationInfo->apiVersion : 0),
             VK_API_VERSION_MINOR(pCreateInfo->pApplicationInfo ? pCreateInfo->pApplicationInfo->apiVersion : 0));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    if (!instance) return;
    void* key = dispatchKey(instance);
    std::unique_ptr<InstanceData> data;
    {
        std::lock_guard<std::mutex> lock(g_mapMutex);
        auto it = g_instances.find(key);
        if (it != g_instances.end()) {
            data = std::move(it->second);
            g_instances.erase(it);
        }
    }
    if (data) data->vt.DestroyInstance(instance, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceLayerProperties(uint32_t* pCount, VkLayerProperties* pProps) {
    if (!pProps) { *pCount = 1; return VK_SUCCESS; }
    if (*pCount < 1) { *pCount = 0; return VK_INCOMPLETE; }
    pProps[0] = g_layerProps;
    *pCount = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pCount,
                                                                    VkExtensionProperties*) {
    if (pLayerName && strcmp(pLayerName, BDEX_LAYER_NAME) == 0) { *pCount = 0; return VK_SUCCESS; }
    return VK_ERROR_LAYER_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceLayerProperties(VkPhysicalDevice, uint32_t* pCount, VkLayerProperties* pProps) {
    return EnumerateInstanceLayerProperties(pCount, pProps);
}

// Present-timing / swapchain-maintenance extensions whose semantics refer to
// the real swapchain we virtualise. We present asynchronously from a private
// queue, so a game driving these (notably VKD3D-Proton for Direct3D 12) hangs;
// hiding them makes it fall back to the basic present path we handle.
bool isHiddenPresentExtension(const char* name) {
    return strcmp(name, "VK_EXT_swapchain_maintenance1") == 0 || strcmp(name, "VK_KHR_swapchain_maintenance1") == 0 ||
           strcmp(name, "VK_KHR_present_id") == 0 || strcmp(name, "VK_KHR_present_wait") == 0 ||
           strcmp(name, "VK_KHR_present_id2") == 0 || strcmp(name, "VK_KHR_present_wait2") == 0 ||
           strcmp(name, "VK_EXT_present_timing") == 0;
}

// What this instance's game does not see. fps_limit_wait (presentWaitOffered):
// present_id and present_wait, which the layer answers for its swapchains,
// stay visible to DXVK / VKD3D-Proton. DXVK enables VK_NV_low_latency2 only
// with present ids: it is hidden from DXVK instead, which keeps Reflex off
// there as before (its markers would name presents that never reach the
// driver). VKD3D-Proton enables it without them: unchanged.
bool hiddenFrom(const InstanceData& inst, const char* name) {
    if (inst.presentWaitOffered) {
        if (strcmp(name, "VK_KHR_present_id") == 0 || strcmp(name, "VK_KHR_present_wait") == 0) return false;
        if (inst.dxvk && strcmp(name, "VK_NV_low_latency2") == 0) return true;
    }
    return isHiddenPresentExtension(name);
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(VkPhysicalDevice physDev, const char* pLayerName,
                                                                  uint32_t* pCount, VkExtensionProperties* pProps) {
    if (pLayerName && strcmp(pLayerName, BDEX_LAYER_NAME) == 0) { *pCount = 0; return VK_SUCCESS; }
    InstanceData* inst = getInstance(dispatchKey(physDev));
    if (!inst) return VK_ERROR_INITIALIZATION_FAILED;
    if (pLayerName || !inst->config.enabled || !inst->config.hidePresentExt)
        return inst->vt.EnumerateDeviceExtensionProperties(physDev, pLayerName, pCount, pProps);

    uint32_t n = 0;
    VkResult r = inst->vt.EnumerateDeviceExtensionProperties(physDev, nullptr, &n, nullptr);
    if (r < 0) return r;
    std::vector<VkExtensionProperties> all(n);
    r = inst->vt.EnumerateDeviceExtensionProperties(physDev, nullptr, &n, all.data());
    if (r < 0) return r;
    std::vector<VkExtensionProperties> keep;
    keep.reserve(all.size());
    for (const auto& e : all)
        if (!hiddenFrom(*inst, e.extensionName)) keep.push_back(e);
    if (!pProps) { *pCount = static_cast<uint32_t>(keep.size()); return VK_SUCCESS; }
    uint32_t m = std::min(*pCount, static_cast<uint32_t>(keep.size()));
    for (uint32_t i = 0; i < m; ++i) pProps[i] = keep[i];
    *pCount = m;
    return m < keep.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// Surface capabilities: when upscaling is on, advertise a reduced current
// extent so the game renders fewer pixels. The layer's real swapchain still
// uses the true display size (createReal queries the driver directly).
// ---------------------------------------------------------------------------

VkExtent2D scaledRenderExtent(VkExtent2D display, float renderScale) {
    // Even dimensions, floored to a small minimum. Not clamped to the surface's
    // minImageExtent: fixed-size (X11) surfaces report min == current == max, so
    // clamping there would undo the reduction. The application's swapchain is
    // virtual (never reaches the driver), so it needs no driver-valid extent.
    uint32_t w = std::max(16u, static_cast<uint32_t>(display.width * renderScale + 0.5f) & ~1u);
    uint32_t h = std::max(16u, static_cast<uint32_t>(display.height * renderScale + 0.5f) & ~1u);
    return {std::min(w, display.width), std::min(h, display.height)};
}

// Reduce the surface size reported to the application so it renders below the
// display resolution; the layer upscales every presented frame back up.
void applyRenderScale(InstanceData* inst, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* caps) {
    const Config& cfg = inst->config;
    if (!cfg.enabled || !inst->renderScaleAllowed.load(std::memory_order_relaxed) || cfg.renderScale >= 0.999f) return;
    if (caps->currentExtent.width != UINT32_MAX && caps->currentExtent.width > 0) {
        // X11 / XWayland: the surface size is fixed, so we can hand back a
        // reduced one directly and the app adopts it.
        VkExtent2D reduced = scaledRenderExtent(caps->currentExtent, cfg.renderScale);
        caps->currentExtent = reduced;
        caps->minImageExtent = {std::min(caps->minImageExtent.width, reduced.width),
                                std::min(caps->minImageExtent.height, reduced.height)};
    } else if (caps->currentExtent.width == UINT32_MAX) {
        // Native Wayland: the app picks its own size, so we cannot reduce it
        // until swapchain.cpp has learned the true window size and flagged the
        // surface. Then we report a concrete reduced extent (pinned via
        // min/max) so even apps that don't follow currentExtent render smaller.
        std::lock_guard<std::mutex> lk(inst->surfMutex);
        auto it = inst->waylandSurfaces.find(surface);
        if (it != inst->waylandSurfaces.end() && it->second.reduce && it->second.displayExtent.width > 0) {
            VkExtent2D reduced = scaledRenderExtent(it->second.displayExtent, cfg.renderScale);
            caps->currentExtent = reduced;
            caps->minImageExtent = reduced;
            caps->maxImageExtent = reduced;
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physDev, VkSurfaceKHR surface,
                                                                       VkSurfaceCapabilitiesKHR* pCaps) {
    InstanceData* inst = getInstance(dispatchKey(physDev));
    if (!inst) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = inst->vt.GetPhysicalDeviceSurfaceCapabilitiesKHR(physDev, surface, pCaps);
    if (r >= 0) applyRenderScale(inst, surface, pCaps);
    return r;
}

// The VK_KHR_get_surface_capabilities2 variant (used by DXVK/VKD3D-Proton and
// others): same reduction, applied to the wrapped VkSurfaceCapabilitiesKHR.
VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice physDev,
                                                                        const VkPhysicalDeviceSurfaceInfo2KHR* pInfo,
                                                                        VkSurfaceCapabilities2KHR* pCaps) {
    InstanceData* inst = getInstance(dispatchKey(physDev));
    if (!inst) return VK_ERROR_INITIALIZATION_FAILED;
    if (!inst->vt.GetPhysicalDeviceSurfaceCapabilities2KHR) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkResult r = inst->vt.GetPhysicalDeviceSurfaceCapabilities2KHR(physDev, pInfo, pCaps);
    if (r >= 0 && pInfo) applyRenderScale(inst, pInfo->surface, &pCaps->surfaceCapabilities);
    return r;
}

// fps_limit_wait: while a frame limit is set, DXVK / VKD3D-Proton are offered
// FIFO alone (InstanceData::fifoOnly) — the mode in which both wait for each
// present (VK_KHR_present_wait, answered by VirtualSwapchain::waitPresent)
// before the game begins a frame, where the limit then holds it. (FIFO is
// always supported.) The surface is noted: the real swapchain keeps the mode
// the game would have picked from the full list (createReal).
VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physDev, VkSurfaceKHR surface,
                                                                      uint32_t* pCount, VkPresentModeKHR* pModes) {
    InstanceData* inst = getInstance(dispatchKey(physDev));
    if (!inst) return VK_ERROR_INITIALIZATION_FAILED;
    if (!inst->fifoOnly.load(std::memory_order_relaxed)) {
        if (inst->presentWaitOffered) {
            std::lock_guard<std::mutex> lk(inst->surfMutex);
            inst->fifoSurfaces.erase(surface);  // (every mode offered again)
        }
        return inst->vt.GetPhysicalDeviceSurfacePresentModesKHR(physDev, surface, pCount, pModes);
    }
    uint32_t n = 0;
    const VkResult r = inst->vt.GetPhysicalDeviceSurfacePresentModesKHR(physDev, surface, &n, nullptr);
    if (r < 0) return r;
    {
        std::lock_guard<std::mutex> lk(inst->surfMutex);
        inst->fifoSurfaces.insert(surface);
    }
    if (!pModes) {
        *pCount = 1;
        return VK_SUCCESS;
    }
    if (*pCount < 1) return VK_INCOMPLETE;
    pModes[0] = VK_PRESENT_MODE_FIFO_KHR;
    *pCount = 1;
    return VK_SUCCESS;
}

#ifdef __linux__
// X11 surfaces: the window each one shows, for the swapchains' X Present
// clock (x11_present_clock.h). The create infos by their layout, without the
// Xlib / xcb headers (VkXlibSurfaceCreateInfoKHR, VkXcbSurfaceCreateInfoKHR).
struct XlibSurfaceCreateInfo {
    VkStructureType sType;
    const void* pNext;
    VkFlags flags;
    void* dpy;
    unsigned long window;
};
struct XcbSurfaceCreateInfo {
    VkStructureType sType;
    const void* pNext;
    VkFlags flags;
    void* connection;
    uint32_t window;
};

template <typename Info>
VkResult createX11Surface(VkInstance instance, const char* name, const Info* pCreateInfo,
                          const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
    InstanceData* inst = getInstance(dispatchKey(instance));
    if (!inst) return VK_ERROR_INITIALIZATION_FAILED;
    using Fn = VkResult(VKAPI_PTR*)(VkInstance, const Info*, const VkAllocationCallbacks*, VkSurfaceKHR*);
    auto next = reinterpret_cast<Fn>(inst->vt.GetInstanceProcAddr(instance, name));
    if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkResult r = next(instance, pCreateInfo, pAllocator, pSurface);
    if (r == VK_SUCCESS && pCreateInfo && pSurface) {
        std::lock_guard<std::mutex> lk(inst->surfMutex);
        inst->x11Windows[*pSurface] = static_cast<uint32_t>(pCreateInfo->window);
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateXlibSurfaceKHR(VkInstance instance, const XlibSurfaceCreateInfo* pCreateInfo,
                                                    const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
    return createX11Surface(instance, "vkCreateXlibSurfaceKHR", pCreateInfo, pAllocator, pSurface);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateXcbSurfaceKHR(VkInstance instance, const XcbSurfaceCreateInfo* pCreateInfo,
                                                   const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
    return createX11Surface(instance, "vkCreateXcbSurfaceKHR", pCreateInfo, pAllocator, pSurface);
}
#endif

// A destroyed surface's handle may come back for another one.
VKAPI_ATTR void VKAPI_CALL DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                                             const VkAllocationCallbacks* pAllocator) {
    InstanceData* inst = getInstance(dispatchKey(instance));
    if (!inst) return;
    if (surface != VK_NULL_HANDLE) {
        std::lock_guard<std::mutex> lk(inst->surfMutex);
        inst->waylandSurfaces.erase(surface);
        inst->x11Windows.erase(surface);
        inst->fifoSurfaces.erase(surface);
        inst->fifoAsked.erase(surface);
    }
    auto next = reinterpret_cast<PFN_vkDestroySurfaceKHR>(inst->vt.GetInstanceProcAddr(instance, "vkDestroySurfaceKHR"));
    if (next) next(instance, surface, pAllocator);
}

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physDev, const VkDeviceCreateInfo* pCreateInfo,
                                            const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    VkLayerDeviceCreateInfo* chain = const_cast<VkLayerDeviceCreateInfo*>(
        findChain<VkLayerDeviceCreateInfo>(pCreateInfo->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO));
    while (chain && chain->function != VK_LAYER_LINK_INFO)
        chain = const_cast<VkLayerDeviceCreateInfo*>(
            findChain<VkLayerDeviceCreateInfo>(chain->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO));
    if (!chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    VkLayerDeviceLink* const below = chain->u.pLayerInfo;

    InstanceData* inst = getInstance(dispatchKey(physDev));
    if (!inst) return VK_ERROR_INITIALIZATION_FAILED;
    auto nextCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(gipa(inst->instance, "vkCreateDevice"));
    if (!nextCreateDevice) return VK_ERROR_INITIALIZATION_FAILED;
    // Every layer below advances the same link chain as it calls down: a
    // retry (without present feedback, without a high-priority queue) must
    // start again from the link after ours — from where the previous attempt
    // left it, gamescope's WSI layer called past itself into the driver's
    // entry point with the wrong chain, and the game crashed.
    auto createDevice = [&](VkPhysicalDevice pd, const VkDeviceCreateInfo* info, const VkAllocationCallbacks* alloc,
                            VkDevice* out) {
        chain->u.pLayerInfo = below;
        return nextCreateDevice(pd, info, alloc, out);
    };

    auto data = std::make_unique<DeviceData>();
    data->inst = inst;
    data->physDev = physDev;
    data->config = Config::load();

    bool hasSwapchainExt = false;
    bool requestedHiddenPresentExt = false;
    bool gameId = false, gameWait = false;  // (fps_limit_wait: offered, answered by the layer)
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i) {
        const char* e = pCreateInfo->ppEnabledExtensionNames[i];
        if (strcmp(e, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) hasSwapchainExt = true;
        if (hiddenFrom(*inst, e)) requestedHiddenPresentExt = true;
        if (strcmp(e, VK_KHR_PRESENT_ID_EXTENSION_NAME) == 0) gameId = true;
        if (strcmp(e, VK_KHR_PRESENT_WAIT_EXTENSION_NAME) == 0) gameWait = true;
        if (strcmp(e, "VK_EXT_swapchain_maintenance1") == 0 || strcmp(e, "VK_KHR_swapchain_maintenance1") == 0)
            data->hasSwapchainMaintenance1 = true;
    }
    {
        const auto* idFeat = findChain<VkPhysicalDevicePresentIdFeaturesKHR>(
            pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR);
        const auto* waitFeat = findChain<VkPhysicalDevicePresentWaitFeaturesKHR>(
            pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR);
        data->gamePresentWait = inst->presentWaitOffered && gameId && gameWait && idFeat && idFeat->presentId &&
                                waitFeat && waitFeat->presentWait;
    }
    // An application may request an extension by name without enumerating it.
    // Its feature chain is const input: changing it in place is invalid, while
    // dropping the extension but leaving its feature enabled can fail device
    // creation. Preserve the application's request and use pass-through.
    if (data->config.enabled && data->config.hidePresentExt && requestedHiddenPresentExt) {
        BDEX_WARN("application explicitly requested present extensions; keeping device and swapchains pass-through");
        data->config.enabled = false;
        inst->renderScaleAllowed.store(false, std::memory_order_relaxed);
    }

    // Queue selection: prefer a compute-capable family with a spare queue so
    // the layer's work never has to serialise with the application's.
    uint32_t nf = 0;
    inst->vt.GetPhysicalDeviceQueueFamilyProperties(physDev, &nf, nullptr);
    data->families.resize(nf);
    inst->vt.GetPhysicalDeviceQueueFamilyProperties(physDev, &nf, data->families.data());

    std::vector<VkDeviceQueueCreateInfo> qcis(pCreateInfo->pQueueCreateInfos,
                                              pCreateInfo->pQueueCreateInfos + pCreateInfo->queueCreateInfoCount);
    std::vector<uint32_t> requested(nf, 0);
    for (const auto& q : qcis) {
        if (q.queueFamilyIndex < nf) requested[q.queueFamilyIndex] += q.queueCount;
        data->appFamilies.push_back(q.queueFamilyIndex);
    }
    std::sort(data->appFamilies.begin(), data->appFamilies.end());
    data->appFamilies.erase(std::unique(data->appFamilies.begin(), data->appFamilies.end()), data->appFamilies.end());

    int chosen = -1;
    std::vector<std::vector<float>> prioStorage;
    // One more queue of family f for the layer, after the application's
    // (and the layer's) others: its index.
    std::vector<uint32_t> added(nf, 0);
    // The queue create info that holds only the layer's queue of a family
    // (none for a family the application uses: its info is shared).
    std::vector<int> ownInfo(nf, -1);
    auto addQueue = [&](uint32_t f) {
        const uint32_t index = requested[f] + added[f]++;
        bool found = false;
        for (auto& q : qcis) {
            if (q.queueFamilyIndex != f || found) continue;
            found = true;
            prioStorage.emplace_back(q.pQueuePriorities, q.pQueuePriorities + q.queueCount);
            prioStorage.back().push_back(1.0f);
            q.queueCount += 1;
            q.pQueuePriorities = prioStorage.back().data();
        }
        if (!found) {
            prioStorage.emplace_back(1, 1.0f);
            VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            q.queueFamilyIndex = f;
            q.queueCount = 1;
            q.pQueuePriorities = prioStorage.back().data();
            ownInfo[f] = static_cast<int>(qcis.size());
            qcis.push_back(q);
        }
        return index;
    };
    auto spare = [&](uint32_t f) { return requested[f] + added[f] < data->families[f].queueCount; };
    for (uint32_t f = 0; f < nf && chosen < 0 && !data->config.sharedQueue; ++f) {
        if (!(data->families[f].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
        if (spare(f)) {
            chosen = static_cast<int>(f);
            data->queueIndex = addQueue(f);
            data->queueShared = false;
        }
    }
    for (uint32_t f = 0; f < nf && chosen < 0; ++f) {
        if (!(data->families[f].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
        if (requested[f] > 0) {
            chosen = static_cast<int>(f);
            data->queueIndex = 0;
            data->queueShared = true;
        }
    }
    if (chosen >= 0) data->queueFamily = static_cast<uint32_t>(chosen);

    // The synthesis on a queue of its own, the copies and presents staying on
    // the queue chosen above: a present's own GPU work (the driver's
    // presentation blit on X11 / Xwayland, for one) then never waits behind
    // the next frame's synthesis, which the pacing now often has queued
    // before it presents (measured: the real frame shown ~9 ms late, half the
    // generated frames replaced, with a 9 ms synthesis; none replaced on two
    // queues).
    // async_compute: that queue belongs to a compute-only family (hardware
    // that runs it alongside the game's graphics work). Otherwise
    // (present_queue, the default) it is a second queue of the same family,
    // when the family has one to spare.
    if (chosen >= 0 && !data->queueShared && data->config.asyncCompute) {
        for (uint32_t f = 0; f < nf; ++f) {
            const VkQueueFlags fl = data->families[f].queueFlags;
            if (int(f) == chosen || !(fl & VK_QUEUE_COMPUTE_BIT) || (fl & VK_QUEUE_GRAPHICS_BIT)) continue;
            if (!spare(f)) continue;
            data->computeFamily = f;
            data->computeIndex = addQueue(f);
            data->asyncCompute = true;
            break;
        }
    }
    if (chosen >= 0 && !data->queueShared && !data->asyncCompute && data->config.presentQueue &&
        spare(static_cast<uint32_t>(chosen))) {
        data->computeFamily = static_cast<uint32_t>(chosen);
        data->computeIndex = addQueue(static_cast<uint32_t>(chosen));
        data->asyncCompute = true;
        data->splitQueue = true;
    }

    VkDeviceCreateInfo ci = *pCreateInfo;
    ci.queueCreateInfoCount = static_cast<uint32_t>(qcis.size());
    ci.pQueueCreateInfos = qcis.data();
    std::vector<const char*> exts(pCreateInfo->ppEnabledExtensionNames,
                                  pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);

    // synth_priority: the synthesis queue, when it has a family (and so a
    // queue create info) of its own — async_compute's compute-only family —
    // at a high global priority, so the GPU runs the generated and real
    // frames' work before a GPU-bound game's next frame. (A family shared
    // with the game can only have one priority for all its queues: not
    // touched.) Some drivers refuse high priority to unprivileged processes:
    // the device is then created without it.
    VkDeviceQueueGlobalPriorityCreateInfoEXT prio{VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_EXT};  // (EXT names: in headers 1.3.204 too)
    prio.globalPriority = VK_QUEUE_GLOBAL_PRIORITY_HIGH_EXT;
    int prioInfo = -1;
    const char* prioExt = nullptr;
    bool prioExtAdded = false;
    if (data->config.enabled && data->config.synthPriority && data->asyncCompute && !data->splitQueue &&
        ownInfo[data->computeFamily] >= 0) {
        uint32_t n = 0;
        inst->vt.EnumerateDeviceExtensionProperties(physDev, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> avail(n);
        inst->vt.EnumerateDeviceExtensionProperties(physDev, nullptr, &n, avail.data());
        for (const char* name : {"VK_KHR_global_priority", "VK_EXT_global_priority"})
            for (const auto& e : avail)
                if (!prioExt && strcmp(e.extensionName, name) == 0) prioExt = name;
        if (prioExt) {
            prioInfo = ownInfo[data->computeFamily];
            prio.pNext = qcis[prioInfo].pNext;
            qcis[prioInfo].pNext = &prio;
            bool have = false;
            for (const char* e : exts) have |= strcmp(e, prioExt) == 0;
            if (!have) exts.push_back(prioExt);
            prioExtAdded = !have;
            ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
            ci.ppEnabledExtensionNames = exts.data();
        }
    }

    // Present feedback for the layer's own swapchains: VK_KHR_present_id +
    // VK_KHR_present_wait tell when each frame is really displayed. They stay
    // hidden from the game (which hangs with them under VKD3D-Proton, see
    // isHiddenPresentExtension); enabled only where the device supports both
    // and the game did not ask for them, and dropped again if device creation
    // fails with them. A game they are offered to (fps_limit_wait) may enable
    // them itself: the feedback then comes with its features (none if it
    // enabled one without the other).
    VkPhysicalDevicePresentIdFeaturesKHR presentIdFeat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    VkPhysicalDevicePresentWaitFeaturesKHR presentWaitFeat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
    bool feedback = false;
    // VK_EXT_present_timing with them (where the driver has it): when each
    // frame's first pixel was really shown, in the steady clock's domain, and
    // the display's refresh duration — not the moment a waiting thread woke.
    bool timing = false;
#if defined(VK_EXT_present_timing) && defined(VK_KHR_present_id2)
    // (It requires VK_KHR_present_id2 and VK_KHR_calibrated_timestamps.)
    VkPhysicalDevicePresentTimingFeaturesEXT timingFeat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT};
    VkPhysicalDevicePresentId2FeaturesKHR id2Feat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR};
#endif
    {
        VkPhysicalDeviceProperties pp{};
        inst->vt.GetPhysicalDeviceProperties(physDev, &pp);
        if (data->config.enabled && data->config.presentFeedback && data->config.hidePresentExt && hasSwapchainExt &&
            !requestedHiddenPresentExt && (!(gameId || gameWait) || data->gamePresentWait) &&
            inst->apiVersion >= VK_API_VERSION_1_1 && pp.apiVersion >= VK_API_VERSION_1_1 && inst->vt.GetPhysicalDeviceFeatures2) {
            uint32_t n = 0;
            inst->vt.EnumerateDeviceExtensionProperties(physDev, nullptr, &n, nullptr);
            std::vector<VkExtensionProperties> avail(n);
            inst->vt.EnumerateDeviceExtensionProperties(physDev, nullptr, &n, avail.data());
            bool hasId = false, hasWait = false, hasTiming = false, hasId2 = false, hasCalibrated = false;
            for (const auto& e : avail) {
                hasId |= strcmp(e.extensionName, VK_KHR_PRESENT_ID_EXTENSION_NAME) == 0;
                hasWait |= strcmp(e.extensionName, VK_KHR_PRESENT_WAIT_EXTENSION_NAME) == 0;
                hasTiming |= strcmp(e.extensionName, "VK_EXT_present_timing") == 0;
                hasId2 |= strcmp(e.extensionName, "VK_KHR_present_id2") == 0;
                hasCalibrated |= strcmp(e.extensionName, "VK_KHR_calibrated_timestamps") == 0;
            }
            hasTiming = hasTiming && hasId2 && hasCalibrated && data->config.presentTiming;
            if (hasId && hasWait) {
                presentIdFeat.pNext = &presentWaitFeat;
#if defined(VK_EXT_present_timing) && defined(VK_KHR_present_id2)
                if (hasTiming) {
                    presentWaitFeat.pNext = &timingFeat;
                    timingFeat.pNext = &id2Feat;
                }
#endif
                VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
                f2.pNext = &presentIdFeat;
                inst->vt.GetPhysicalDeviceFeatures2(physDev, &f2);
                feedback = presentIdFeat.presentId && presentWaitFeat.presentWait;
#if defined(VK_EXT_present_timing) && defined(VK_KHR_present_id2)
                timing = feedback && hasTiming && timingFeat.presentTiming && id2Feat.presentId2;
#endif
                presentWaitFeat.pNext = nullptr;
            }
        }
    }
    // With present feedback when possible, without it otherwise.
    auto create = [&]() {
        VkResult res = VK_ERROR_INITIALIZATION_FAILED;
        data->presentFeedback = false;
        data->presentTiming = false;
        if (feedback) {
            VkDeviceCreateInfo fci = ci;
            std::vector<const char*> fexts(ci.ppEnabledExtensionNames, ci.ppEnabledExtensionNames + ci.enabledExtensionCount);
            // (the game's own present_id / present_wait: its names and features as they are)
            const bool own = !data->gamePresentWait;
            if (own) {
                fexts.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
                fexts.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
            }
            auto chain = [&](bool withTiming) {
                void* next = const_cast<void*>(ci.pNext);
#if defined(VK_EXT_present_timing) && defined(VK_KHR_present_id2)
                if (withTiming) {
                    id2Feat.pNext = next;
                    timingFeat.pNext = &id2Feat;
                    next = &timingFeat;
                }
#else
                (void)withTiming;
#endif
                if (own) {
                    presentIdFeat.presentId = VK_TRUE;
                    presentWaitFeat.presentWait = VK_TRUE;
                    presentWaitFeat.pNext = next;
                    presentIdFeat.pNext = &presentWaitFeat;
                    next = &presentIdFeat;
                }
                fci.pNext = next;
            };
#if defined(VK_EXT_present_timing) && defined(VK_KHR_present_id2)
            if (timing) {
                fexts.push_back("VK_KHR_present_id2");
                fexts.push_back("VK_KHR_calibrated_timestamps");
                fexts.push_back("VK_EXT_present_timing");
                timingFeat = VkPhysicalDevicePresentTimingFeaturesEXT{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT};
                timingFeat.presentTiming = VK_TRUE;
                id2Feat = VkPhysicalDevicePresentId2FeaturesKHR{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR};
                id2Feat.presentId2 = VK_TRUE;
            }
#endif
            chain(timing);
            fci.enabledExtensionCount = static_cast<uint32_t>(fexts.size());
            fci.ppEnabledExtensionNames = fexts.data();
            res = createDevice(physDev, &fci, pAllocator, pDevice);
            if (res != VK_SUCCESS && timing && !(res == VK_ERROR_NOT_PERMITTED_EXT && prioInfo >= 0)) {
                // (present timing is the newest piece: without it first)
                BDEX_WARN("device creation with present timing failed (%s); retrying without it", vkResultName(res));
                timing = false;
                fexts.resize(fexts.size() - 3);
                chain(false);
                fci.enabledExtensionCount = static_cast<uint32_t>(fexts.size());
                fci.ppEnabledExtensionNames = fexts.data();
                res = createDevice(physDev, &fci, pAllocator, pDevice);
            }
            // (a refused queue priority is not the feedback's doing: the
            // caller drops the priority and keeps the feedback)
            if (res == VK_ERROR_NOT_PERMITTED_EXT && prioInfo >= 0) return res;
            if (res != VK_SUCCESS) BDEX_WARN("device creation with present feedback failed (%s); retrying without", vkResultName(res));
            else {
                data->presentFeedback = true;
                data->presentTiming = timing;
            }
        }
        if (!data->presentFeedback) res = createDevice(physDev, &ci, pAllocator, pDevice);
        return res;
    };
    VkResult r = create();
    if (r != VK_SUCCESS && prioInfo >= 0) {
        // VK_ERROR_NOT_PERMITTED: the usual answer without privileges
        // (NVIDIA; amdgpu wants CAP_SYS_NICE above normal priority).
        if (r == VK_ERROR_NOT_PERMITTED_EXT)
            BDEX_INFO("high-priority synthesis queue not permitted to this process; normal priority");
        else
            BDEX_WARN("device creation with a high-priority synthesis queue failed (%s); retrying without", vkResultName(r));
        qcis[prioInfo].pNext = prio.pNext;
        prioInfo = -1;
        if (prioExtAdded) exts.pop_back();
        ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
        ci.ppEnabledExtensionNames = exts.data();
        r = create();
    }
    if (r != VK_SUCCESS) return r;
    data->synthPriorityHigh = prioInfo >= 0;

    data->device = *pDevice;
    data->vt.load(*pDevice, gdpa);
    // gamescope's WSI layer answers vkGetRefreshCycleDurationGOOGLE for its
    // swapchains whether or not the extension is enabled (it never reaches
    // the driver, which may not have it: NVIDIA's has not).
    data->gamescope = gamescopeWsi() && data->vt.GetRefreshCycleDurationGOOGLE != nullptr;
    if (gamescopeWsi())
        BDEX_INFO("presenting through gamescope's WSI layer: %s", data->gamescope ? "refresh rate and frame limit from gamescope"
                                                                                  : "its refresh cycle is not available");
    inst->vt.GetPhysicalDeviceProperties(physDev, &data->props);
    if (inst->apiVersion >= VK_API_VERSION_1_1 && data->props.apiVersion >= VK_API_VERSION_1_1 &&
        inst->vt.GetPhysicalDeviceProperties2) {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &id;
        inst->vt.GetPhysicalDeviceProperties2(physDev, &p2);
        memcpy(data->deviceUuid, id.deviceUUID, VK_UUID_SIZE);
        data->haveUuid = true;
    }
    inst->vt.GetPhysicalDeviceMemoryProperties(physDev, &data->memProps);
    data->config.autoConfigure(data->props, data->memProps);
    data->setFrameLimitHint();
    if (!data->config.autoTunedTo.empty()) {
        static const char* kind[] = {"other", "integrated GPU", "discrete GPU", "virtual GPU", "software"};
        BDEX_INFO("auto-tuned for '%s' (%s): preset %s", data->props.deviceName,
                  kind[data->props.deviceType <= 4 ? data->props.deviceType : 0], data->config.autoTunedTo.c_str());
    }

    if (chosen >= 0 && hasSwapchainExt && data->vt.CreateSwapchainKHR) {
        data->vt.GetDeviceQueue(*pDevice, data->queueFamily, data->queueIndex, &data->queue);
        if (data->queue) adoptDispatch(data->queue, *pDevice);
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = data->queueFamily;
        if (data->vt.CreateCommandPool(*pDevice, &pi, nullptr, &data->cmdPool) == VK_SUCCESS) data->layerUsable = true;
        if (data->asyncCompute) {
            data->vt.GetDeviceQueue(*pDevice, data->computeFamily, data->computeIndex, &data->computeQueue);
            if (data->computeQueue) adoptDispatch(data->computeQueue, *pDevice);
            else data->asyncCompute = false;
        }
    }
    if (data->asyncCompute && data->splitQueue)
        BDEX_INFO("synthesis on queue %u, copies and presents on queue %u (family %u)", data->computeIndex,
                  data->queueIndex, data->queueFamily);
    else if (data->asyncCompute)
        BDEX_INFO("async compute: synthesis on queue family %u%s, presentation on family %u", data->computeFamily,
                  data->synthPriorityHigh ? " (high priority)" : "", data->queueFamily);
    // Crash journal (crash.h), only where the layer is at work: never in a
    // game it stays out of (anti-cheat), never where it is pass-through.
    if (data->config.enabled && !data->config.antiCheatDisabled && data->config.crashLog &&
        crashLogInstall(platformCacheDir(), platformExeName(),
                        std::string(BDEX_LAYER_NAME " v" BDEX_LAYER_VERSION_STR " on '") + data->props.deviceName +
                            "', " + data->config.describe()))
        BDEX_INFO("crash journal: %s (should the layer crash)", crashLogPath().c_str());
    if (data->config.enabled || data->config.antiCheatDisabled) {
        StatusEntry e;
        e.kind = "device";
        e.state = data->config.antiCheatDisabled ? "anti-cheat" : data->layerUsable ? "active" : "inactive";
        if (data->config.antiCheatDisabled) e.reason = "a multiplayer anti-cheat is running: the layer stays off";
        else if (!data->layerUsable) e.reason = !hasSwapchainExt ? "the device does not use VK_KHR_swapchain" : "no usable queue";
        e.device = data->props.deviceName;
        e.antiCheat = data->config.antiCheatIgnored;
        statusSet(statusKey(*pDevice), e);
    }
    BDEX_INFO("device '%s': layer %s (queue family %u index %u, %s%s)", data->props.deviceName,
              data->layerUsable ? "active" : "inactive", data->queueFamily, data->queueIndex,
              data->queueShared ? "shared with the application" : "private",
              data->presentTiming ? ", present feedback + timing" : data->presentFeedback ? ", present feedback" : "");
    if (!hasSwapchainExt) BDEX_DBG("VK_KHR_swapchain not enabled on this device");
    if (data->gamePresentWait)
        BDEX_INFO("fps_limit_wait: the game waits for its presents (VK_KHR_present_wait, answered by the layer): "
                  "a frame limit holds it there");
    else if (inst->presentWaitOffered && hasSwapchainExt)
        BDEX_INFO("fps_limit_wait: the game did not enable VK_KHR_present_id and VK_KHR_present_wait%s: a frame limit "
                  "holds its present thread",
                  gameId || gameWait ? "" : " (the driver may not have them)");

    std::lock_guard<std::mutex> lock(g_mapMutex);
    g_devices[dispatchKey(*pDevice)] = std::move(data);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    if (!device) return;
    std::unique_ptr<DeviceData> data;
    {
        std::lock_guard<std::mutex> lock(g_mapMutex);
        auto it = g_devices.find(dispatchKey(device));
        if (it != g_devices.end()) {
            data = std::move(it->second);
            g_devices.erase(it);
        }
    }
    if (!data) return;
    {
        std::lock_guard<std::mutex> lock(data->swapchainsMutex);
        if (!data->swapchains.empty()) BDEX_WARN("%zu swapchain(s) still alive at device destruction", data->swapchains.size());
        data->swapchains.clear();
    }
    statusRemove(statusKey(device));
    if (data->cmdPool) data->vt.DestroyCommandPool(device, data->cmdPool, nullptr);
    data->destroyPipelineCache();
    data->vt.DestroyDevice(device, pAllocator);
}

// ---------------------------------------------------------------------------
// Queue serialisation (needed when we share a queue with the application)
// ---------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* pSubmits, VkFence fence) {
    DeviceData* dev = getDevice(dispatchKey(queue));
    std::lock_guard<std::mutex> lock(dev->queueMutex(queue));
    return dev->vt.QueueSubmit(queue, count, pSubmits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* pSubmits, VkFence fence) {
    DeviceData* dev = getDevice(dispatchKey(queue));
    std::lock_guard<std::mutex> lock(dev->queueMutex(queue));
    return dev->vt.QueueSubmit2(queue, count, pSubmits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2KHR(VkQueue queue, uint32_t count, const VkSubmitInfo2* pSubmits, VkFence fence) {
    DeviceData* dev = getDevice(dispatchKey(queue));
    std::lock_guard<std::mutex> lock(dev->queueMutex(queue));
    return dev->vt.QueueSubmit2KHR(queue, count, pSubmits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL QueueBindSparse(VkQueue queue, uint32_t count, const VkBindSparseInfo* pInfo, VkFence fence) {
    DeviceData* dev = getDevice(dispatchKey(queue));
    std::lock_guard<std::mutex> lock(dev->queueMutex(queue));
    return dev->vt.QueueBindSparse(queue, count, pInfo, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL QueueWaitIdle(VkQueue queue) {
    DeviceData* dev = getDevice(dispatchKey(queue));
    std::lock_guard<std::mutex> lock(dev->queueMutex(queue));
    return dev->vt.QueueWaitIdle(queue);
}

VKAPI_ATTR VkResult VKAPI_CALL DeviceWaitIdle(VkDevice device) {
    DeviceData* dev = getDevice(dispatchKey(device));
    std::vector<std::mutex*> mutexes;
    {
        std::lock_guard<std::mutex> lock(dev->queueMapMutex);
        for (auto& kv : dev->queueMutexes) mutexes.push_back(kv.second.get());
    }
    std::vector<std::unique_lock<std::mutex>> locks;
    for (std::mutex* m : mutexes) locks.emplace_back(*m);
    return dev->vt.DeviceWaitIdle(device);
}

// ---------------------------------------------------------------------------
// Swapchain
// ---------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                  const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {
    DeviceData* dev = getDevice(dispatchKey(device));
    Config cfg;  // a copy: another thread's present may be reloading the live settings
    uint64_t cfgVersion = UINT64_MAX;
    dev->snapshotConfig(cfg, cfgVersion);
    bool virtualise = dev->layerUsable && cfg.enabled && pAllocator == nullptr;
    // Why this swapchain is passed through, for the control panel (status.h).
    std::string reason = !dev->layerUsable ? "no usable queue on this device"
                         : !cfg.enabled   ? "frame generation disabled"
                         : pAllocator     ? "custom allocator"
                                          : "";
    if (virtualise && pCreateInfo->imageArrayLayers != 1) { BDEX_WARN("multi-layer swapchain: passing through"); virtualise = false; reason = "multi-layer swapchain"; }
    if (virtualise && (pCreateInfo->flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR)) { BDEX_WARN("protected swapchain: passing through"); virtualise = false; reason = "protected swapchain"; }
    if (virtualise && !encodingForFormat(pCreateInfo->imageFormat).supported) {
        BDEX_WARN("swapchain format %d not supported: passing through", static_cast<int>(pCreateInfo->imageFormat));
        virtualise = false;
        reason = "swapchain format " + std::to_string(static_cast<int>(pCreateInfo->imageFormat)) + " not supported";
    }
    if (virtualise && !(pCreateInfo->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR)) {
        // Check the usages promised to the application before creating our
        // real swapchain. A surface's supportedUsageFlags need not be valid
        // for ordinary images of the same format. If they are not, leave the
        // original swapchain path intact (including oldSwapchain handling).
        // Mutable-format lists need the full vkCreateImage pNext query, so
        // those rely on createVirtualImages' actual creation/fallback below.
        VkImageFormatProperties props{};
        VkResult support = dev->inst->vt.GetPhysicalDeviceImageFormatProperties(
            dev->physDev, pCreateInfo->imageFormat, VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL, pCreateInfo->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            0, &props);
        if (support != VK_SUCCESS || pCreateInfo->imageExtent.width > props.maxExtent.width ||
            pCreateInfo->imageExtent.height > props.maxExtent.height ||
            !(props.sampleCounts & VK_SAMPLE_COUNT_1_BIT)) {
            BDEX_WARN("swapchain image usage 0x%x cannot be preserved in virtual images: passing through",
                      pCreateInfo->imageUsage);
            virtualise = false;
            reason = "image usage not available on plain images";
        }
    }
    if (virtualise && !dev->ensurePresentQueue(pCreateInfo->surface)) {
        BDEX_WARN("no compute queue can present to this surface: passing through");
        virtualise = false;
        reason = "no compute queue can present to this surface";
    }
    if (pCreateInfo->oldSwapchain) {
        if (VirtualSwapchain* old = dev->findSwapchain(pCreateInfo->oldSwapchain)) old->retire();
    }
    if (virtualise) {
        try {
            VirtualSwapchainPtr vs(new VirtualSwapchain(*dev, *pCreateInfo),
                                   [](VirtualSwapchain* p) { delete p; });
            *pSwapchain = vs->handle();
            std::lock_guard<std::mutex> lock(dev->swapchainsMutex);
            dev->swapchains.insert_or_assign(*pSwapchain, std::move(vs));
            return VK_SUCCESS;
        } catch (const VkError& e) {
            BDEX_ERR("virtual swapchain creation failed (%s): passing through", e.what());
            reason = std::string("virtual swapchain creation failed: ") + e.what();
        } catch (const std::exception& e) {
            BDEX_ERR("virtual swapchain creation failed (%s): passing through", e.what());
            reason = std::string("virtual swapchain creation failed: ") + e.what();
        }
    }
    const VkResult r = dev->vt.CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    // Reported only where frame generation was wanted: a disabled layer
    // (every other Vulkan program) leaves no status file behind.
    if (r == VK_SUCCESS && (cfg.enabled || cfg.antiCheatDisabled)) {
        StatusEntry e;
        e.kind = "swapchain";
        e.state = "pass-through";
        e.reason = cfg.antiCheatDisabled ? "anti-cheat detected" : reason;
        e.width = pCreateInfo->imageExtent.width;
        e.height = pCreateInfo->imageExtent.height;
        e.device = dev->props.deviceName;
        statusSet(statusKey(*pSwapchain), e);
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator) {
    DeviceData* dev = getDevice(dispatchKey(device));
    if (swapchain) {
        VirtualSwapchainPtr vs(nullptr, nullptr);
        {
            std::lock_guard<std::mutex> lock(dev->swapchainsMutex);
            auto it = dev->swapchains.find(swapchain);
            if (it != dev->swapchains.end()) {
                vs = std::move(it->second);
                dev->swapchains.erase(it);
            }
        }
        statusRemove(statusKey(swapchain));
        if (vs) {
            vs.reset();  // waits for pending work and destroys the real swapchain
            return;
        }
    }
    dev->vt.DestroySwapchainKHR(device, swapchain, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pCount, VkImage* pImages) {
    DeviceData* dev = getDevice(dispatchKey(device));
    if (VirtualSwapchain* vs = dev->findSwapchain(swapchain)) return vs->getImages(pCount, pImages);
    return dev->vt.GetSwapchainImagesKHR(device, swapchain, pCount, pImages);
}

VKAPI_ATTR VkResult VKAPI_CALL AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                                   VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex) {
    DeviceData* dev = getDevice(dispatchKey(device));
    if (VirtualSwapchain* vs = dev->findSwapchain(swapchain)) return vs->acquire(timeout, semaphore, fence, pImageIndex);
    return dev->vt.AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
}

VKAPI_ATTR VkResult VKAPI_CALL AcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* pInfo, uint32_t* pImageIndex) {
    DeviceData* dev = getDevice(dispatchKey(device));
    if (VirtualSwapchain* vs = dev->findSwapchain(pInfo->swapchain))
        return vs->acquire(pInfo->timeout, pInfo->semaphore, pInfo->fence, pImageIndex);
    return dev->vt.AcquireNextImage2KHR(device, pInfo, pImageIndex);
}

VKAPI_ATTR VkResult VKAPI_CALL ReleaseSwapchainImagesEXT(VkDevice device, const VkReleaseSwapchainImagesInfoEXT* pInfo) {
    DeviceData* dev = getDevice(dispatchKey(device));
    if (VirtualSwapchain* vs = dev->findSwapchain(pInfo->swapchain)) return vs->release(pInfo->imageIndexCount, pInfo->pImageIndices);
    if (!dev->vt.ReleaseSwapchainImagesEXT) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return dev->vt.ReleaseSwapchainImagesEXT(device, pInfo);
}

// The application holds our real swapchain handle, so it may call these on it.
// We present asynchronously from the worker, so the application must not block
// on the driver's present feedback for a swapchain we drive ourselves.
VKAPI_ATTR VkResult VKAPI_CALL WaitForPresentKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t presentId,
                                                 uint64_t timeout) {
    DeviceData* dev = getDevice(dispatchKey(device));
    if (VirtualSwapchain* vs = dev->findSwapchain(swapchain)) {
        // fps_limit_wait: the game's present ids are ours to answer (see
        // VirtualSwapchain::waitPresent); otherwise (hide_present_ext = 0) our
        // asynchronous presentation: do not let the game stall on it.
        if (dev->gamePresentWait) return vs->waitPresent(presentId, timeout);
        return VK_SUCCESS;
    }
    if (!dev->vt.WaitForPresentKHR) return VK_SUCCESS;
    return dev->vt.WaitForPresentKHR(device, swapchain, presentId, timeout);
}

VKAPI_ATTR VkResult VKAPI_CALL GetSwapchainStatusKHR(VkDevice device, VkSwapchainKHR swapchain) {
    DeviceData* dev = getDevice(dispatchKey(device));
    if (dev->findSwapchain(swapchain)) return VK_SUCCESS;
    if (!dev->vt.GetSwapchainStatusKHR) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return dev->vt.GetSwapchainStatusKHR(device, swapchain);
}

#ifdef VK_KHR_present_wait2
VKAPI_ATTR VkResult VKAPI_CALL WaitForPresent2KHR(VkDevice device, VkSwapchainKHR swapchain,
                                                  const VkPresentWait2InfoKHR* pInfo) {
    DeviceData* dev = getDevice(dispatchKey(device));
    if (dev->findSwapchain(swapchain)) return VK_SUCCESS;  // as WaitForPresentKHR
    return dev->vt.WaitForPresent2KHR(device, swapchain, pInfo);
}
#endif

// The other calls on a swapchain we virtualise go to the real swapchain —
// the handle the application holds, unless the surface changed size since:
// followSurface re-created it underneath, and the application's handle is
// then a destroyed swapchain (HDR games call vkSetHdrMetadataEXT again after
// a mode change).
VkSwapchainKHR currentReal(DeviceData* dev, VkSwapchainKHR swapchain) {
    if (VirtualSwapchain* vs = dev->findSwapchain(swapchain)) return vs->currentReal();
    return swapchain;
}

VKAPI_ATTR void VKAPI_CALL SetHdrMetadataEXT(VkDevice device, uint32_t swapchainCount, const VkSwapchainKHR* pSwapchains,
                                             const VkHdrMetadataEXT* pMetadata) {
    DeviceData* dev = getDevice(dispatchKey(device));
    std::vector<VkSwapchainKHR> real(pSwapchains, pSwapchains + swapchainCount);
    for (VkSwapchainKHR& s : real) s = currentReal(dev, s);
    dev->vt.SetHdrMetadataEXT(device, swapchainCount, real.data(), pMetadata);
}

VKAPI_ATTR VkResult VKAPI_CALL GetRefreshCycleDurationGOOGLE(VkDevice device, VkSwapchainKHR swapchain,
                                                             VkRefreshCycleDurationGOOGLE* pProperties) {
    DeviceData* dev = getDevice(dispatchKey(device));
    return dev->vt.GetRefreshCycleDurationGOOGLE(device, currentReal(dev, swapchain), pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL GetPastPresentationTimingGOOGLE(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pCount,
                                                               VkPastPresentationTimingGOOGLE* pTimings) {
    DeviceData* dev = getDevice(dispatchKey(device));
    return dev->vt.GetPastPresentationTimingGOOGLE(device, currentReal(dev, swapchain), pCount, pTimings);
}

VKAPI_ATTR VkResult VKAPI_CALL GetSwapchainCounterEXT(VkDevice device, VkSwapchainKHR swapchain,
                                                      VkSurfaceCounterFlagBitsEXT counter, uint64_t* pValue) {
    DeviceData* dev = getDevice(dispatchKey(device));
    return dev->vt.GetSwapchainCounterEXT(device, currentReal(dev, swapchain), counter, pValue);
}

VKAPI_ATTR void VKAPI_CALL SetLocalDimmingAMD(VkDevice device, VkSwapchainKHR swapchain, VkBool32 enable) {
    DeviceData* dev = getDevice(dispatchKey(device));
    dev->vt.SetLocalDimmingAMD(device, currentReal(dev, swapchain), enable);
}

#ifdef VK_NV_low_latency2
// fps_limit with Reflex on: Reflex's own limiter (minimumIntervalUs, the
// driver's) holds the game, the longer of the game's interval and the
// layer's — where the game's own limit holds it (Unreal hands its limit to
// Reflex). Never a sleep of the layer's in vkLatencySleepNV: VKD3D-Proton
// makes that call holding the lock its every latency marker takes, the
// game's render and present threads' too (low_latency_swapchain_lock; it
// waits for the sleep after it). Held there, those threads waited out the
// layer's sleep: until 31/10, Oblivion Remastered at 30 fps went from the
// start of a frame's simulation to its present in 66.7 ms, two frames,
// against 14.5 with its own limit (its Reflex reports).
static uint32_t reflexIntervalUs(DeviceData* dev) {
    const double fps = dev->capFps.load(std::memory_order_relaxed);
    const uint32_t ours = fps > 0.0 ? uint32_t(std::lround(1e6 / fps)) : 0u;
    return std::max(dev->appMinIntervalUs.load(std::memory_order_relaxed), ours);
}

VKAPI_ATTR VkResult VKAPI_CALL SetLatencySleepModeNV(VkDevice device, VkSwapchainKHR swapchain,
                                                     const VkLatencySleepModeInfoNV* pInfo) {
    DeviceData* dev = getDevice(dispatchKey(device));
    dev->appLowLatencyMode.store(pInfo && pInfo->lowLatencyMode, std::memory_order_relaxed);
    dev->appLowLatencyBoost.store(pInfo && pInfo->lowLatencyBoost, std::memory_order_relaxed);
    dev->appMinIntervalUs.store(pInfo ? pInfo->minimumIntervalUs : 0u, std::memory_order_relaxed);
    if (!pInfo || !pInfo->lowLatencyMode) dev->appPacedAtMs.store(0.0, std::memory_order_relaxed);
    const VkSwapchainKHR real = currentReal(dev, swapchain);
    if (!pInfo || !pInfo->lowLatencyMode) {
        dev->sleepModeSentOn = VK_NULL_HANDLE;
        return dev->vt.SetLatencySleepModeNV(device, real, pInfo);
    }
    VkLatencySleepModeInfoNV mode = *pInfo;
    mode.minimumIntervalUs = reflexIntervalUs(dev);
    const VkResult r = dev->vt.SetLatencySleepModeNV(device, real, &mode);
    dev->sleepModeSentUs = mode.minimumIntervalUs;
    dev->sleepModeSentOn = r == VK_SUCCESS ? real : VK_NULL_HANDLE;
    return r;
}

// The game's Reflex reports, medians of the frames it reported: from its
// simulation's start to its present call and to its GPU work's end, as the
// driver timed them (the layer's latency is after those).
static void readReflexReports(DeviceData* dev, VkDevice device, VkSwapchainKHR real) {
    if (!dev->vt.GetLatencyTimingsNV) return;
    std::vector<VkLatencyTimingsFrameReportNV> reports(64, {VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV});
    VkGetLatencyMarkerInfoNV info{VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV};
    info.timingCount = uint32_t(reports.size());
    info.pTimings = reports.data();
    dev->vt.GetLatencyTimingsNV(device, real, &info);
    std::vector<double> present, gpu;
    for (uint32_t i = 0; i < std::min<uint32_t>(info.timingCount, uint32_t(reports.size())); ++i) {
        const VkLatencyTimingsFrameReportNV& r = reports[i];
        if (!r.simStartTimeUs) continue;
        if (r.presentStartTimeUs > r.simStartTimeUs && r.presentStartTimeUs - r.simStartTimeUs < 1'000'000)
            present.push_back(double(r.presentStartTimeUs - r.simStartTimeUs) / 1000.0);
        if (r.gpuRenderEndTimeUs > r.simStartTimeUs && r.gpuRenderEndTimeUs - r.simStartTimeUs < 1'000'000)
            gpu.push_back(double(r.gpuRenderEndTimeUs - r.simStartTimeUs) / 1000.0);
    }
    auto median = [](std::vector<double>& v) {
        if (v.empty()) return -1.0;
        std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
        return v[v.size() / 2];
    };
    dev->reflexSimToPresentMs.store(median(present), std::memory_order_relaxed);
    dev->reflexSimToGpuMs.store(median(gpu), std::memory_order_relaxed);
}

VKAPI_ATTR VkResult VKAPI_CALL LatencySleepNV(VkDevice device, VkSwapchainKHR swapchain, const VkLatencySleepInfoNV* pInfo) {
    DeviceData* dev = getDevice(dispatchKey(device));
    if (dev->appLowLatencyMode.load(std::memory_order_relaxed)) {
        // (In the game's own low-latency call: what it serialises its calls with covers this one.)
        if (const double now = steadyMs(); now - dev->reflexReadAtMs >= 1000.0) {
            dev->reflexReadAtMs = now;
            readReflexReports(dev, device, currentReal(dev, swapchain));
        }
    }
    const VkSwapchainKHR real = currentReal(dev, swapchain);
    if (dev->appLowLatencyMode.load(std::memory_order_relaxed)) {
        dev->appPacedAtMs.store(steadyMs(), std::memory_order_relaxed);
        dev->appLatencyApi.store(1, std::memory_order_relaxed);
        // fps_limit here, before the game reads its input: Reflex's limiter
        // set to it (a new limit, a real swapchain re-created underneath),
        // within the game's own call (reflexIntervalUs).
        if (const uint32_t want = reflexIntervalUs(dev); want != dev->sleepModeSentUs || real != dev->sleepModeSentOn) {
            VkLatencySleepModeInfoNV mode{VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV};
            mode.lowLatencyMode = VK_TRUE;
            mode.lowLatencyBoost = dev->appLowLatencyBoost.load(std::memory_order_relaxed);
            mode.minimumIntervalUs = want;
            const VkResult r = dev->vt.SetLatencySleepModeNV(device, real, &mode);
            dev->sleepModeSentUs = want;
            dev->sleepModeSentOn = r == VK_SUCCESS ? real : VK_NULL_HANDLE;
        }
        if (dev->capFps.load(std::memory_order_relaxed) > 0.0 && dev->sleepModeSentOn) {
            dev->capAtSleepMs.store(steadyMs(), std::memory_order_relaxed);
            if (!dev->capAtSleepLogged.exchange(true))
                BDEX_INFO("fps_limit: the game is held by its Reflex sleep (Reflex's own limiter), before it reads its input");
        }
    }
    return dev->vt.LatencySleepNV(device, real, pInfo);
}

VKAPI_ATTR void VKAPI_CALL SetLatencyMarkerNV(VkDevice device, VkSwapchainKHR swapchain,
                                              const VkSetLatencyMarkerInfoNV* pInfo) {
    DeviceData* dev = getDevice(dispatchKey(device));
    dev->vt.SetLatencyMarkerNV(device, currentReal(dev, swapchain), pInfo);
}

VKAPI_ATTR void VKAPI_CALL GetLatencyTimingsNV(VkDevice device, VkSwapchainKHR swapchain,
                                               VkGetLatencyMarkerInfoNV* pInfo) {
    DeviceData* dev = getDevice(dispatchKey(device));
    dev->vt.GetLatencyTimingsNV(device, currentReal(dev, swapchain), pInfo);
}
#endif

#ifdef VK_AMD_anti_lag
VKAPI_ATTR void VKAPI_CALL AntiLagUpdateAMD(VkDevice device, const VkAntiLagDataAMD* pData) {
    DeviceData* dev = getDevice(dispatchKey(device));
    if (pData && pData->mode == VK_ANTI_LAG_MODE_ON_AMD) {
        dev->appPacedAtMs.store(steadyMs(), std::memory_order_relaxed);
        dev->appLatencyApi.store(2, std::memory_order_relaxed);
        // fps_limit at the input stage: before the game reads its input.
        if (pData->pPresentationInfo && pData->pPresentationInfo->stage == VK_ANTI_LAG_STAGE_INPUT_AMD) {
            dev->capAtSleep();
            if (dev->capFps.load(std::memory_order_relaxed) > 0.0 && !dev->capAtSleepLogged.exchange(true))
                BDEX_INFO("fps_limit: the game is held at Anti-Lag's input stage, before it reads its input");
        }
    }
    dev->vt.AntiLagUpdateAMD(device, pData);
}
#endif

VkResult worstResult(VkResult a, VkResult b) {
    if (a < 0) return a;
    if (b < 0) return b;
    return a != VK_SUCCESS ? a : b;
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    DeviceData* dev = getDevice(dispatchKey(queue));
    const uint32_t n = pPresentInfo->swapchainCount;
    std::vector<VirtualSwapchain*> virt(n, nullptr);
    uint32_t nVirtual = 0;
    if (dev->layerUsable) {
        for (uint32_t i = 0; i < n; ++i) {
            virt[i] = dev->findSwapchain(pPresentInfo->pSwapchains[i]);
            if (virt[i]) ++nVirtual;
        }
    }
    if (nVirtual == 0) {
        std::lock_guard<std::mutex> lock(dev->queueMutex(queue));
        return dev->vt.QueuePresentKHR(queue, pPresentInfo);
    }
    if (n == 1) {
        VkResult r = virt[0]->present(pPresentInfo->pImageIndices[0], *pPresentInfo, true);
        if (pPresentInfo->pResults) pPresentInfo->pResults[0] = r;
        return r;
    }

    VkResult result = VK_SUCCESS;
    bool semaphoresConsumed = false;

    // Build every reduced pNext chain before submitting anything. Vulkan's
    // per-swapchain arrays must use the same indexing as the reduced list,
    // including when the only real swapchain was not first in the app call.
    std::vector<uint32_t> realPositions;
    std::vector<std::unique_ptr<PresentSlice>> virtualPresents(n);
    for (uint32_t i = 0; i < n; ++i)
        if (!virt[i]) realPositions.push_back(i);
    std::unique_ptr<PresentSlice> realPresent;
    if (!realPositions.empty()) {
        realPresent = std::make_unique<PresentSlice>(*pPresentInfo, realPositions, true);
        if (!realPresent->valid()) {
            BDEX_ERR("cannot split mixed present: %s", realPresent->error());
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
    }
    bool firstVirtual = realPositions.empty();
    for (uint32_t i = 0; i < n; ++i) {
        if (!virt[i]) continue;
        virtualPresents[i] = std::make_unique<PresentSlice>(*pPresentInfo, std::vector<uint32_t>{i}, firstVirtual);
        firstVirtual = false;
        if (!virtualPresents[i]->valid()) {
            BDEX_ERR("cannot split virtual present: %s", virtualPresents[i]->error());
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
    }

    if (nVirtual < n) {
        // Mixed present: forward the real swapchains in one call (it consumes
        // the wait semaphores), then handle ours without them.
        static bool warned = false;
        if (!warned) { BDEX_WARN("present mixes layer and pass-through swapchains; synchronisation is best-effort"); warned = true; }
        {
            std::lock_guard<std::mutex> lock(dev->queueMutex(queue));
            result = dev->vt.QueuePresentKHR(queue, &realPresent->info());
        }
        if (pPresentInfo->pResults)
            for (size_t k = 0; k < realPositions.size(); ++k)
                pPresentInfo->pResults[realPositions[k]] = realPresent->results()[k];
        semaphoresConsumed = true;
    }

    for (uint32_t i = 0; i < n; ++i) {
        if (!virt[i]) continue;
        VkResult r = virt[i]->present(pPresentInfo->pImageIndices[i], virtualPresents[i]->info(), !semaphoresConsumed);
        semaphoresConsumed = true;
        if (pPresentInfo->pResults) pPresentInfo->pResults[i] = r;
        result = worstResult(result, r);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

struct Hook {
    const char* name;
    PFN_vkVoidFunction fn;
};

#define HOOK(name) {"vk" #name, reinterpret_cast<PFN_vkVoidFunction>(name)}

const Hook g_globalHooks[] = {
    HOOK(CreateInstance),
    HOOK(EnumerateInstanceLayerProperties),
    HOOK(EnumerateInstanceExtensionProperties),
};

const Hook g_instanceHooks[] = {
    HOOK(DestroyInstance),
    HOOK(EnumerateDeviceLayerProperties),
    HOOK(EnumerateDeviceExtensionProperties),
    HOOK(CreateDevice),
    HOOK(GetPhysicalDeviceSurfaceCapabilitiesKHR),
    HOOK(GetPhysicalDeviceSurfaceCapabilities2KHR),
    HOOK(GetPhysicalDeviceSurfacePresentModesKHR),
    HOOK(DestroySurfaceKHR),
#ifdef __linux__
    HOOK(CreateXlibSurfaceKHR),
    HOOK(CreateXcbSurfaceKHR),
#endif
};

const Hook g_deviceHooks[] = {
    HOOK(DestroyDevice),
    HOOK(QueueSubmit),
    HOOK(QueueSubmit2),
    HOOK(QueueSubmit2KHR),
    HOOK(QueueBindSparse),
    HOOK(QueueWaitIdle),
    HOOK(DeviceWaitIdle),
    HOOK(CreateSwapchainKHR),
    HOOK(DestroySwapchainKHR),
    HOOK(GetSwapchainImagesKHR),
    HOOK(AcquireNextImageKHR),
    HOOK(AcquireNextImage2KHR),
    HOOK(QueuePresentKHR),
    HOOK(WaitForPresentKHR),
    HOOK(GetSwapchainStatusKHR),
    HOOK(ReleaseSwapchainImagesEXT),
    {"vkReleaseSwapchainImagesKHR", reinterpret_cast<PFN_vkVoidFunction>(ReleaseSwapchainImagesEXT)},
    HOOK(SetHdrMetadataEXT),
    HOOK(GetRefreshCycleDurationGOOGLE),
    HOOK(GetPastPresentationTimingGOOGLE),
    HOOK(GetSwapchainCounterEXT),
    HOOK(SetLocalDimmingAMD),
#ifdef VK_NV_low_latency2
    HOOK(SetLatencySleepModeNV),
    HOOK(LatencySleepNV),
    HOOK(SetLatencyMarkerNV),
    HOOK(GetLatencyTimingsNV),
#endif
#ifdef VK_KHR_present_wait2
    HOOK(WaitForPresent2KHR),
#endif
#ifdef VK_AMD_anti_lag
    HOOK(AntiLagUpdateAMD),
#endif
};

template <size_t N>
PFN_vkVoidFunction findHook(const Hook (&hooks)[N], const char* name) {
    for (const Hook& h : hooks)
        if (strcmp(h.name, name) == 0) return h.fn;
    return nullptr;
}

} // namespace
} // namespace bdex

namespace bdex {
namespace {

// Internal versions of the two entry points: the exported names below can be
// interposed by libvulkan's own symbols when the layer is dlopen'ed into a
// process that links the loader, so all internal references use these.
PFN_vkVoidFunction VKAPI_CALL layerGetDeviceProcAddr(VkDevice device, const char* pName);

// The loader dispatches physical-device functions (vkGetPhysicalDevice*)
// through this entry point, so a layer that only intercepts them via
// GetInstanceProcAddr is skipped for them. Surface capabilities are hooked
// here for the render-scale override.
PFN_vkVoidFunction VKAPI_CALL layerGetPhysicalDeviceProcAddr(VkInstance instance, const char* pName) {
    if (pName && strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceSurfaceCapabilitiesKHR);
    if (pName && strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilities2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceSurfaceCapabilities2KHR);
    if (pName && strcmp(pName, "vkGetPhysicalDeviceSurfacePresentModesKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceSurfacePresentModesKHR);
    InstanceData* inst = instance ? getInstance(dispatchKey(instance)) : nullptr;
    if (!inst) return nullptr;
    return inst->vt.GetInstanceProcAddr(instance, pName);
}

PFN_vkVoidFunction VKAPI_CALL layerGetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (!pName) return nullptr;
    if (strcmp(pName, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(layerGetInstanceProcAddr);
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(layerGetDeviceProcAddr);
    if (PFN_vkVoidFunction f = findHook(g_globalHooks, pName)) return f;
    if (!instance) return nullptr;
    InstanceData* inst = getInstance(dispatchKey(instance));
    if (!inst) return nullptr;
    PFN_vkVoidFunction next = inst->vt.GetInstanceProcAddr(instance, pName);
    if (!next) return nullptr;  // not supported below us: stay invisible
    if (PFN_vkVoidFunction f = findHook(g_instanceHooks, pName)) return f;
    if (PFN_vkVoidFunction f = findHook(g_deviceHooks, pName)) return f;
    return next;
}

PFN_vkVoidFunction VKAPI_CALL layerGetDeviceProcAddr(VkDevice device, const char* pName) {
    if (!pName) return nullptr;
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(layerGetDeviceProcAddr);
    if (!device) return nullptr;
    DeviceData* dev = getDevice(dispatchKey(device));
    if (!dev) return nullptr;
    PFN_vkVoidFunction next = dev->vt.GetDeviceProcAddr(device, pName);
    if (!next) return nullptr;
    if (PFN_vkVoidFunction f = findHook(g_deviceHooks, pName)) return f;
    return next;
}

} // namespace
} // namespace bdex

// vulkan_core.h already declares vkGetInstanceProcAddr/vkGetDeviceProcAddr
// (plain extern "C"), so MSVC rejects a redeclaration with dllexport
// ("different linkage"): the Windows exports come from bdex_framegen.def
// instead, which also maps the __stdcall-decorated 32-bit names.
#ifdef _WIN32
#define BDEX_EXPORT extern "C"
#else
#define BDEX_EXPORT extern "C" __attribute__((visibility("default")))
#endif

BDEX_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    return bdex::layerGetInstanceProcAddr(instance, pName);
}

BDEX_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    return bdex::layerGetDeviceProcAddr(device, pName);
}

BDEX_EXPORT VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    if (!pVersionStruct || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
    if (pVersionStruct->loaderLayerInterfaceVersion < 2) return VK_ERROR_INITIALIZATION_FAILED;
    pVersionStruct->loaderLayerInterfaceVersion = 2;
    pVersionStruct->pfnGetInstanceProcAddr = bdex::layerGetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = bdex::layerGetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = bdex::layerGetPhysicalDeviceProcAddr;
    return VK_SUCCESS;
}
