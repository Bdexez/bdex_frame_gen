#include "device.h"
#include "log.h"
#include "vk_util.h"

#include <filesystem>

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
    img = AllocatedImage{};
}

AllocatedBuffer DeviceData::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible) {
    AllocatedBuffer buf;
    buf.size = size;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
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
    if (configStamp == -1 || stamp == configStamp) { configStamp = stamp; return; }
    configStamp = stamp;
    Config fresh = Config::load();
    fresh.autoConfigure(props, memProps);
    const bool wasPaused = config.paused;
    config.applyLive(fresh);
    ++configVersion;
    BDEX_INFO("config file changed: live settings reloaded%s", config.paused != wasPaused
                  ? (config.paused ? " (frame generation paused)" : " (frame generation resumed)") : "");
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
