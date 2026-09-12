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

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(VkPhysicalDevice physDev, const char* pLayerName,
                                                                  uint32_t* pCount, VkExtensionProperties* pProps) {
    if (pLayerName && strcmp(pLayerName, BDEX_LAYER_NAME) == 0) { *pCount = 0; return VK_SUCCESS; }
    InstanceData* inst = getInstance(dispatchKey(physDev));
    if (!inst) return VK_ERROR_INITIALIZATION_FAILED;
    return inst->vt.EnumerateDeviceExtensionProperties(physDev, pLayerName, pCount, pProps);
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

    VkResult r = createDevice(physDev, &ci, pAllocator, pDevice);
    if (r != VK_SUCCESS) return r;

    data->device = *pDevice;
    data->vt.load(*pDevice, gdpa);
    inst->vt.GetPhysicalDeviceProperties(physDev, &data->props);
    inst->vt.GetPhysicalDeviceMemoryProperties(physDev, &data->memProps);

    if (chosen >= 0 && hasSwapchainExt && data->vt.CreateSwapchainKHR) {
        data->vt.GetDeviceQueue(*pDevice, data->queueFamily, data->queueIndex, &data->queue);
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
            auto vs = std::make_unique<VirtualSwapchain>(*dev, *pCreateInfo);
            *pSwapchain = vs->handle();
            std::lock_guard<std::mutex> lock(dev->swapchainsMutex);
            dev->swapchains[*pSwapchain] = std::move(vs);
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
        std::unique_ptr<VirtualSwapchain> vs;
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
PFN_vkVoidFunction layerGetDeviceProcAddr(VkDevice device, const char* pName);

PFN_vkVoidFunction layerGetInstanceProcAddr(VkInstance instance, const char* pName) {
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

PFN_vkVoidFunction layerGetDeviceProcAddr(VkDevice device, const char* pName) {
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

#define BDEX_EXPORT extern "C" __attribute__((visibility("default")))

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
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}
