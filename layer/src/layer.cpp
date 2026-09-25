// Vulkan layer entry points: instance/device lifecycle, dispatch and the
// swapchain hooks that route through VirtualSwapchain.
#include "config.h"
#include "status.h"
#include "device.h"
#include "dispatch.h"
#include "log.h"
#include "present_slice.h"
#include "swapchain.h"
#include "vk_util.h"

#include <algorithm>
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
           strcmp(name, "VK_KHR_present_id2") == 0 || strcmp(name, "VK_KHR_present_wait2") == 0;
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
        if (!isHiddenPresentExtension(e.extensionName)) keep.push_back(e);
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

    InstanceData* inst = getInstance(dispatchKey(physDev));
    if (!inst) return VK_ERROR_INITIALIZATION_FAILED;
    auto createDevice = reinterpret_cast<PFN_vkCreateDevice>(gipa(inst->instance, "vkCreateDevice"));
    if (!createDevice) return VK_ERROR_INITIALIZATION_FAILED;

    auto data = std::make_unique<DeviceData>();
    data->inst = inst;
    data->physDev = physDev;
    data->config = Config::load();

    bool hasSwapchainExt = false;
    bool requestedHiddenPresentExt = false;
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i) {
        const char* e = pCreateInfo->ppEnabledExtensionNames[i];
        if (strcmp(e, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) hasSwapchainExt = true;
        if (isHiddenPresentExtension(e)) requestedHiddenPresentExt = true;
        if (strcmp(e, "VK_EXT_swapchain_maintenance1") == 0 || strcmp(e, "VK_KHR_swapchain_maintenance1") == 0)
            data->hasSwapchainMaintenance1 = true;
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
    for (uint32_t f = 0; f < nf && chosen < 0 && !data->config.sharedQueue; ++f) {
        if (!(data->families[f].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
        if (requested[f] < data->families[f].queueCount) {
            chosen = static_cast<int>(f);
            data->queueIndex = requested[f];
            data->queueShared = false;
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
                qcis.push_back(q);
            }
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

    // async_compute: the synthesis goes to a queue of a compute-only family
    // (hardware that runs it alongside the game's graphics work), the copies
    // and presents stay on the queue chosen above. Only with a spare queue in
    // such a family, and a different family from the main queue.
    if (chosen >= 0 && data->config.asyncCompute) {
        for (uint32_t f = 0; f < nf; ++f) {
            const VkQueueFlags fl = data->families[f].queueFlags;
            if (int(f) == chosen || !(fl & VK_QUEUE_COMPUTE_BIT) || (fl & VK_QUEUE_GRAPHICS_BIT)) continue;
            if (requested[f] >= data->families[f].queueCount) continue;
            data->computeFamily = f;
            data->computeIndex = requested[f];
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
                qcis.push_back(q);
            }
            data->asyncCompute = true;
            break;
        }
    }

    VkDeviceCreateInfo ci = *pCreateInfo;
    ci.queueCreateInfoCount = static_cast<uint32_t>(qcis.size());
    ci.pQueueCreateInfos = qcis.data();

    // Present feedback for the layer's own swapchains: VK_KHR_present_id +
    // VK_KHR_present_wait tell when each frame is really displayed. They stay
    // hidden from the game (which hangs with them under VKD3D-Proton, see
    // isHiddenPresentExtension); enabled only where the device supports both
    // and the game did not ask for them, and dropped again if device creation
    // fails with them.
    std::vector<const char*> exts(pCreateInfo->ppEnabledExtensionNames,
                                  pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);
    VkPhysicalDevicePresentIdFeaturesKHR presentIdFeat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    VkPhysicalDevicePresentWaitFeaturesKHR presentWaitFeat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
    bool feedback = false;
    {
        VkPhysicalDeviceProperties pp{};
        inst->vt.GetPhysicalDeviceProperties(physDev, &pp);
        if (data->config.enabled && data->config.presentFeedback && data->config.hidePresentExt && hasSwapchainExt &&
            !requestedHiddenPresentExt && inst->apiVersion >= VK_API_VERSION_1_1 &&
            pp.apiVersion >= VK_API_VERSION_1_1 && inst->vt.GetPhysicalDeviceFeatures2) {
            uint32_t n = 0;
            inst->vt.EnumerateDeviceExtensionProperties(physDev, nullptr, &n, nullptr);
            std::vector<VkExtensionProperties> avail(n);
            inst->vt.EnumerateDeviceExtensionProperties(physDev, nullptr, &n, avail.data());
            bool hasId = false, hasWait = false;
            for (const auto& e : avail) {
                hasId |= strcmp(e.extensionName, VK_KHR_PRESENT_ID_EXTENSION_NAME) == 0;
                hasWait |= strcmp(e.extensionName, VK_KHR_PRESENT_WAIT_EXTENSION_NAME) == 0;
            }
            if (hasId && hasWait) {
                presentIdFeat.pNext = &presentWaitFeat;
                VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
                f2.pNext = &presentIdFeat;
                inst->vt.GetPhysicalDeviceFeatures2(physDev, &f2);
                feedback = presentIdFeat.presentId && presentWaitFeat.presentWait;
            }
        }
    }
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (feedback) {
        VkDeviceCreateInfo fci = ci;
        std::vector<const char*> fexts = exts;
        fexts.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
        fexts.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
        fci.enabledExtensionCount = static_cast<uint32_t>(fexts.size());
        fci.ppEnabledExtensionNames = fexts.data();
        presentIdFeat.presentId = VK_TRUE;
        presentWaitFeat.presentWait = VK_TRUE;
        presentIdFeat.pNext = &presentWaitFeat;
        presentWaitFeat.pNext = const_cast<void*>(fci.pNext);
        fci.pNext = &presentIdFeat;
        r = createDevice(physDev, &fci, pAllocator, pDevice);
        if (r != VK_SUCCESS) BDEX_WARN("device creation with present feedback failed (%s); retrying without", vkResultName(r));
        else data->presentFeedback = true;
    }
    if (!data->presentFeedback) r = createDevice(physDev, &ci, pAllocator, pDevice);
    if (r != VK_SUCCESS) return r;

    data->device = *pDevice;
    data->vt.load(*pDevice, gdpa);
    inst->vt.GetPhysicalDeviceProperties(physDev, &data->props);
    inst->vt.GetPhysicalDeviceMemoryProperties(physDev, &data->memProps);
    data->config.autoConfigure(data->props, data->memProps);
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
    if (data->asyncCompute)
        BDEX_INFO("async compute: synthesis on queue family %u, presentation on family %u", data->computeFamily,
                  data->queueFamily);
    if (data->config.enabled || data->config.antiCheatDisabled) {
        StatusEntry e;
        e.kind = "device";
        e.state = data->config.antiCheatDisabled ? "anti-cheat" : data->layerUsable ? "active" : "inactive";
        if (data->config.antiCheatDisabled) e.reason = "a multiplayer anti-cheat is running: the layer stays off";
        else if (!data->layerUsable) e.reason = !hasSwapchainExt ? "the device does not use VK_KHR_swapchain" : "no usable queue";
        e.device = data->props.deviceName;
        statusSet(statusKey(*pDevice), e);
    }
    BDEX_INFO("device '%s': layer %s (queue family %u index %u, %s%s)", data->props.deviceName,
              data->layerUsable ? "active" : "inactive", data->queueFamily, data->queueIndex,
              data->queueShared ? "shared with the application" : "private",
              data->presentFeedback ? ", present feedback" : "");
    if (!hasSwapchainExt) BDEX_DBG("VK_KHR_swapchain not enabled on this device");

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
    if (dev->findSwapchain(swapchain)) return VK_SUCCESS;  // our async presentation; do not let the game stall on it
    if (!dev->vt.WaitForPresentKHR) return VK_SUCCESS;
    return dev->vt.WaitForPresentKHR(device, swapchain, presentId, timeout);
}

VKAPI_ATTR VkResult VKAPI_CALL GetSwapchainStatusKHR(VkDevice device, VkSwapchainKHR swapchain) {
    DeviceData* dev = getDevice(dispatchKey(device));
    if (dev->findSwapchain(swapchain)) return VK_SUCCESS;
    if (!dev->vt.GetSwapchainStatusKHR) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return dev->vt.GetSwapchainStatusKHR(device, swapchain);
}

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
