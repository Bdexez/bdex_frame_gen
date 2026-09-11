#include "device.h"
#include "log.h"
#include "swapchain.h"
#include "vk_util.h"

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

VkResult DeviceData::submit(const VkSubmitInfo& info, VkFence fence) {
    std::lock_guard<std::mutex> lock(queueMutex(queue));
    return vt.QueueSubmit(queue, 1, &info, fence);
}

} // namespace bdex
