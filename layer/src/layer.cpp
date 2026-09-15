// Vulkan layer entry points: instance/device lifecycle, dispatch and the
// swapchain hooks that route through VirtualSwapchain.
#include "config.h"
#include "device.h"
#include "dispatch.h"
#include "log.h"
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
std::unordered_map<void*, std::unique_ptr<InstanceData>> g_instances;
std::unordered_map<void*, std::unique_ptr<DeviceData>> g_devices;
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
    if (!cfg.enabled || cfg.renderScale >= 0.999f) return;
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
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i) {
        const char* e = pCreateInfo->ppEnabledExtensionNames[i];
        if (strcmp(e, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) hasSwapchainExt = true;
        if (strcmp(e, "VK_EXT_swapchain_maintenance1") == 0 || strcmp(e, "VK_KHR_swapchain_maintenance1") == 0)
            data->hasSwapchainMaintenance1 = true;
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

    VkDeviceCreateInfo ci = *pCreateInfo;
    ci.queueCreateInfoCount = static_cast<uint32_t>(qcis.size());
    ci.pQueueCreateInfos = qcis.data();

    // Force-disable the present-timing extensions (swapchain_maintenance1,
    // present_id/wait): filtering them from EnumerateDeviceExtensionProperties
    // is not always honoured by the loader, so also strip them from the enabled
    // list and clear their feature bits. VKD3D-Proton (Direct3D 12) then uses
    // the basic present path, which our virtualised swapchain supports.
    std::vector<const char*> devExts;
    if (data->config.enabled && data->config.hidePresentExt) {
        for (uint32_t i = 0; i < ci.enabledExtensionCount; ++i) {
            if (isHiddenPresentExtension(ci.ppEnabledExtensionNames[i])) {
                BDEX_INFO("disabling %s (incompatible with the virtualised swapchain)", ci.ppEnabledExtensionNames[i]);
                data->hasSwapchainMaintenance1 = false;
                continue;
            }
            devExts.push_back(ci.ppEnabledExtensionNames[i]);
        }
        ci.enabledExtensionCount = static_cast<uint32_t>(devExts.size());
        ci.ppEnabledExtensionNames = devExts.data();
        for (auto* s = static_cast<VkBaseOutStructure*>(const_cast<void*>(ci.pNext)); s; s = s->pNext) {
            if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR)
                reinterpret_cast<VkPhysicalDevicePresentIdFeaturesKHR*>(s)->presentId = VK_FALSE;
            else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR)
                reinterpret_cast<VkPhysicalDevicePresentWaitFeaturesKHR*>(s)->presentWait = VK_FALSE;
            else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT)
                reinterpret_cast<VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT*>(s)->swapchainMaintenance1 = VK_FALSE;
        }
    }

    VkResult r = createDevice(physDev, &ci, pAllocator, pDevice);
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
    }
    BDEX_INFO("device '%s': layer %s (queue family %u index %u, %s)", data->props.deviceName,
              data->layerUsable ? "active" : "inactive", data->queueFamily, data->queueIndex,
              data->queueShared ? "shared with the application" : "private");
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
    const Config& cfg = dev->config;
    bool virtualise = dev->layerUsable && cfg.enabled && pAllocator == nullptr;
    if (virtualise && pCreateInfo->imageArrayLayers != 1) { BDEX_WARN("multi-layer swapchain: passing through"); virtualise = false; }
    if (virtualise && (pCreateInfo->flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR)) { BDEX_WARN("protected swapchain: passing through"); virtualise = false; }
    if (virtualise && !encodingForFormat(pCreateInfo->imageFormat).supported) {
        BDEX_WARN("swapchain format %d not supported: passing through", static_cast<int>(pCreateInfo->imageFormat));
        virtualise = false;
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
        } catch (const std::exception& e) {
            BDEX_ERR("virtual swapchain creation failed (%s): passing through", e.what());
        }
    }
    return dev->vt.CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
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

    VkResult result = VK_SUCCESS;
    bool semaphoresConsumed = false;

    if (nVirtual < n) {
        // Mixed present: forward the real swapchains in one call (it consumes
        // the wait semaphores), then handle ours without them.
        static bool warned = false;
        if (!warned) { BDEX_WARN("present mixes layer and pass-through swapchains; synchronisation is best-effort"); warned = true; }
        std::vector<VkSwapchainKHR> scs;
        std::vector<uint32_t> idx;
        std::vector<uint32_t> map;
        for (uint32_t i = 0; i < n; ++i) {
            if (virt[i]) continue;
            scs.push_back(pPresentInfo->pSwapchains[i]);
            idx.push_back(pPresentInfo->pImageIndices[i]);
            map.push_back(i);
        }
        std::vector<VkResult> results(scs.size());
        VkPresentInfoKHR pi = *pPresentInfo;
        pi.swapchainCount = static_cast<uint32_t>(scs.size());
        pi.pSwapchains = scs.data();
        pi.pImageIndices = idx.data();
        pi.pResults = results.data();
        {
            std::lock_guard<std::mutex> lock(dev->queueMutex(queue));
            result = dev->vt.QueuePresentKHR(queue, &pi);
        }
        if (pPresentInfo->pResults)
            for (size_t k = 0; k < map.size(); ++k) pPresentInfo->pResults[map[k]] = results[k];
        semaphoresConsumed = true;
    }

    for (uint32_t i = 0; i < n; ++i) {
        if (!virt[i]) continue;
        VkResult r = virt[i]->present(pPresentInfo->pImageIndices[i], *pPresentInfo, !semaphoresConsumed);
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
