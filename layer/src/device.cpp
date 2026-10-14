#include "device.h"
#include "log.h"
#include "platform.h"
#include "vk_util.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace bdex {

std::mutex& DeviceData::queueMutex(VkQueue q) {
    std::lock_guard<std::mutex> lock(queueMapMutex);
    auto& m = queueMutexes[q];
    if (!m) m = std::make_unique<std::mutex>();
    return *m;
}

VirtualSwapchain* DeviceData::findSwapchain(VkSwapchainKHR sc) {
    std::lock_guard<std::mutex> lock(swapchainsMutex);
    auto it = swapchains.find(sc);
    return it == swapchains.end() ? nullptr : it->second.get();
}

uint32_t DeviceData::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags flags) const {
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & flags) == flags) return i;
    // Fall back to any compatible type.
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
        if (typeBits & (1u << i)) return i;
    throw VkError(VK_ERROR_OUT_OF_DEVICE_MEMORY, "no suitable memory type");
}

bool DeviceData::formatSupports(VkFormat format, VkFormatFeatureFlags features) const {
    VkFormatProperties fp{};
    inst->vt.GetPhysicalDeviceFormatProperties(physDev, format, &fp);
    return (fp.optimalTilingFeatures & features) == features;
}

bool DeviceData::ensurePresentQueue(VkSurfaceKHR surface) {
    VkBool32 supported = VK_FALSE;
    if (inst->vt.GetPhysicalDeviceSurfaceSupportKHR(physDev, queueFamily, surface, &supported) == VK_SUCCESS && supported)
        return true;
    // A compute-only spare queue is not necessarily presentation-capable.
    // Queue resources are device-wide, so switch only before any virtual
    // swapchain has allocated command buffers from the current pool.
    std::lock_guard<std::mutex> lock(swapchainsMutex);
    if (!swapchains.empty()) return false;
    for (uint32_t f : appFamilies) {
        if (f >= families.size() || !(families[f].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
        supported = VK_FALSE;
        if (inst->vt.GetPhysicalDeviceSurfaceSupportKHR(physDev, f, surface, &supported) != VK_SUCCESS || !supported)
            continue;
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = f;
        VkCommandPool replacement = VK_NULL_HANDLE;
        if (vt.CreateCommandPool(device, &pi, nullptr, &replacement) != VK_SUCCESS) continue;
        VkQueue fallback = VK_NULL_HANDLE;
        vt.GetDeviceQueue(device, f, 0, &fallback);
        if (!fallback) { vt.DestroyCommandPool(device, replacement, nullptr); continue; }
        adoptDispatch(fallback, device);
        vt.DestroyCommandPool(device, cmdPool, nullptr);
        cmdPool = replacement;
        queue = fallback;
        queueFamily = f;
        queueIndex = 0;
        queueShared = true;
        BDEX_INFO("layer queue switched to application family %u for surface presentation", f);
        return true;
    }
    return false;
}

AllocatedImage DeviceData::createImage(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage, bool withView,
                                       VkImageCreateFlags flags, const void* pNext,
                                       const std::vector<uint32_t>* sharingFamilies) {
    AllocatedImage img;
    img.format = format;
    img.extent = extent;

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.pNext = pNext;
    ci.flags = flags;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {extent.width, extent.height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // The layer's internal images are used by both of its queues under
    // async_compute: share them between the two families.
    const std::vector<uint32_t> ours = layerFamilies();
    if (!sharingFamilies && ours.size() > 1) sharingFamilies = &ours;
    if (sharingFamilies && sharingFamilies->size() > 1) {
        ci.sharingMode = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = static_cast<uint32_t>(sharingFamilies->size());
        ci.pQueueFamilyIndices = sharingFamilies->data();
    }
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    try {
        VK_CHECK(vt.CreateImage(device, &ci, nullptr, &img.image));

        VkMemoryRequirements req{};
        vt.GetImageMemoryRequirements(device, img.image, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vt.AllocateMemory(device, &ai, nullptr, &img.memory));
        img.allocated = req.size;
        memoryInUse += int64_t(req.size);
        VK_CHECK(vt.BindImageMemory(device, img.image, img.memory, 0));

        if (withView) {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = img.image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = format;
            vi.subresourceRange = colorRange();
            VK_CHECK(vt.CreateImageView(device, &vi, nullptr, &img.view));
        }
    } catch (...) {
        destroyImage(img);
        throw;
    }
    return img;
}

void DeviceData::destroyImage(AllocatedImage& img) {
    if (img.view) vt.DestroyImageView(device, img.view, nullptr);
    if (img.image) vt.DestroyImage(device, img.image, nullptr);
    if (img.memory) vt.FreeMemory(device, img.memory, nullptr);
    memoryInUse -= int64_t(img.allocated);
    img = AllocatedImage{};
}

namespace {
std::string pipelineCacheFile(const VkPhysicalDeviceProperties& p) {
    char name[96];
    int n = snprintf(name, sizeof name, "/pipelines-%04x-%04x-", p.vendorID, p.deviceID);
    for (uint8_t b : p.pipelineCacheUUID) n += snprintf(name + n, sizeof name - size_t(n), "%02x", b);
    // (the 32-bit layer keeps a file of its own: its driver build is another)
    snprintf(name + n, sizeof name - size_t(n), sizeof(void*) == 4 ? "-32.bin" : ".bin");
    return platformCacheDir() + name;
}
} // namespace

VkPipelineCache DeviceData::pipelineCache() {
    std::lock_guard<std::mutex> lock(pipelineCacheMutex);
    if (pipelineCacheTried_ || !config.pipelineCache || !vt.CreatePipelineCache) return pipelineCache_;
    pipelineCacheTried_ = true;
    std::vector<char> data;
    {
        std::ifstream in(pipelineCacheFile(props), std::ios::binary);
        if (in) data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    // (the header's UUID must be this driver's; a driver also ignores data it
    // does not recognise, but some have crashed on stale caches)
    if (data.size() < 32 || memcmp(data.data() + 16, props.pipelineCacheUUID, VK_UUID_SIZE) != 0) data.clear();
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    if (vt.CreatePipelineCache(device, &ci, nullptr, &pipelineCache_) != VK_SUCCESS) {
        ci.initialDataSize = 0;
        ci.pInitialData = nullptr;
        if (vt.CreatePipelineCache(device, &ci, nullptr, &pipelineCache_) != VK_SUCCESS) pipelineCache_ = VK_NULL_HANDLE;
    }
    pipelineCacheSaved_ = data.size();
    return pipelineCache_;
}

void DeviceData::savePipelineCache() {
    std::lock_guard<std::mutex> lock(pipelineCacheMutex);
    if (!pipelineCache_ || !vt.GetPipelineCacheData) return;
    size_t size = 0;
    if (vt.GetPipelineCacheData(device, pipelineCache_, &size, nullptr) != VK_SUCCESS || size == pipelineCacheSaved_ ||
        size > (64u << 20))
        return;
    std::vector<char> data(size);
    if (vt.GetPipelineCacheData(device, pipelineCache_, &size, data.data()) != VK_SUCCESS) return;
    // (a temporary name per process: two games may save at once; the rename is atomic)
    const std::string path = pipelineCacheFile(props), tmp = path + ".tmp" + std::to_string(platformPid());
    std::error_code ec;
    std::filesystem::create_directories(platformCacheDir(), ec);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out.write(data.data(), std::streamsize(size));
        if (!out) return;
    }
    if (std::rename(tmp.c_str(), path.c_str()) == 0) pipelineCacheSaved_ = size;
}

void DeviceData::destroyPipelineCache() {
    savePipelineCache();
    std::lock_guard<std::mutex> lock(pipelineCacheMutex);
    if (pipelineCache_) vt.DestroyPipelineCache(device, pipelineCache_, nullptr);
    pipelineCache_ = VK_NULL_HANDLE;
}

AllocatedBuffer DeviceData::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible) {
    AllocatedBuffer buf;
    buf.size = size;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    const std::vector<uint32_t> ours = layerFamilies();  // see createImage
    if (ours.size() > 1) {
        ci.sharingMode = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = static_cast<uint32_t>(ours.size());
        ci.pQueueFamilyIndices = ours.data();
    }
    try {
        VK_CHECK(vt.CreateBuffer(device, &ci, nullptr, &buf.buffer));
        VkMemoryRequirements req{};
        vt.GetBufferMemoryRequirements(device, buf.buffer, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, hostVisible
                                                ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                                : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vt.AllocateMemory(device, &ai, nullptr, &buf.memory));
        buf.allocated = req.size;
        memoryInUse += int64_t(req.size);
        VK_CHECK(vt.BindBufferMemory(device, buf.buffer, buf.memory, 0));
    } catch (...) {
        destroyBuffer(buf);
        throw;
    }
    return buf;
}

void DeviceData::destroyBuffer(AllocatedBuffer& buf) {
    if (buf.buffer) vt.DestroyBuffer(device, buf.buffer, nullptr);
    if (buf.memory) vt.FreeMemory(device, buf.memory, nullptr);
    memoryInUse -= int64_t(buf.allocated);
    buf = AllocatedBuffer{};
}

void DeviceData::pollConfig() {
    std::lock_guard<std::mutex> lock(configMutex);
    const auto now = std::chrono::steady_clock::now();
    if (now - configChecked < std::chrono::milliseconds(500)) return;
    configChecked = now;
    int64_t stamp = -2;
    std::error_code ec;
    const std::string path = Config::filePath();
    if (!path.empty()) {
        const auto t = std::filesystem::last_write_time(path, ec);
        if (!ec) stamp = static_cast<int64_t>(t.time_since_epoch().count());
    }
    // require_app: « Démarrer » / « Arrêter » in the control panel start and
    // stop the generation in this process (Config::load reads the session).
    bool targeted = config.appTargeted;
    double panelHz = config.panelDisplayHz;
    if (config.requireApp) {
        const AppSession s = platformAppSession();
        targeted = s.target != 0 && s.target == platformPid();
        panelHz = s.displayHz;
    }
    const bool fileChanged = configStamp != -1 && stamp != configStamp;
    configStamp = stamp;
    if (!fileChanged && targeted == config.appTargeted) {
        if (panelHz != config.panelDisplayHz) {  // the panel read the monitor (again)
            config.panelDisplayHz = panelHz;
            ++configVersion;
        }
        return;
    }
    Config fresh = Config::load();
    fresh.autoConfigure(props, memProps);
    const bool wasPaused = config.paused;
    Config next = config.keepingCreationSettings(fresh);
    if (config.requireApp && !next.sameOutputSettings(config)) {
        // Multiplier, mode, flow, HUD...: the swapchains rebuild their output
        // stage (VirtualSwapchain::present), as the panel applies them live.
        ++next.rebuildGen;
        config = next;
    } else {
        config.applyLive(fresh);
        config.appTargeted = fresh.appTargeted;
    }
    config.panelDisplayHz = panelHz;
    ++configVersion;
    BDEX_INFO("%s%s", fileChanged ? "config file changed: live settings reloaded" : "control panel: generation target changed",
              config.paused != wasPaused ? (config.paused ? " (frame generation paused)" : " (frame generation resumed)") : "");
}

void DeviceData::snapshotConfig(Config& out, uint64_t& version) {
    std::lock_guard<std::mutex> lock(configMutex);
    if (version == configVersion) return;
    out = config;
    version = configVersion;
}

VkResult DeviceData::allocateCommandBuffer(VkCommandBuffer* cmd, VkCommandPool pool) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool ? pool : cmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkResult r = vt.AllocateCommandBuffers(device, &ai, cmd);
    if (r >= 0) adoptDispatch(*cmd, device);
    return r;
}

VkResult DeviceData::submit(const VkSubmitInfo& info, VkFence fence) {
    std::lock_guard<std::mutex> lock(queueMutex(queue));
    return vt.QueueSubmit(queue, 1, &info, fence);
}

} // namespace bdex
