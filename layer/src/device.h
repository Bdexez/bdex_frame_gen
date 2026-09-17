#pragma once
#include "config.h"
#include "dispatch.h"
#include "vk_util.h"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace bdex {

class VirtualSwapchain;
// Owning pointer with a runtime deleter, so DeviceData (and device.cpp) can
// be compiled without the VirtualSwapchain definition: the capture product
// reuses DeviceData/FrameGen without the layer's swapchain virtualisation.
using VirtualSwapchainPtr = std::unique_ptr<VirtualSwapchain, void (*)(VirtualSwapchain*)>;

// Per-surface state for native-Wayland upscaling. On Wayland the surface never
// reports its size (currentExtent is always UINT32_MAX), so the render-scale
// hook cannot reduce it up front. Instead the first swapchain creation records
// the true window size here and bounces the app through one recreation; the
// caps hook then reports a reduced currentExtent so the app renders smaller.
struct WaylandSurfaceState {
    VkExtent2D displayExtent{};   // true window size, learned at CreateSwapchainKHR
    bool reduce = false;          // report a reduced currentExtent for this surface
};

struct InstanceData {
    InstanceDispatch vt;
    VkInstance instance = VK_NULL_HANDLE;
    uint32_t apiVersion = VK_API_VERSION_1_0;
    Config config;  // loaded at instance creation; the render-scale hook needs it before the device exists
    std::mutex surfMutex;
    std::unordered_map<VkSurfaceKHR, WaylandSurfaceState> waylandSurfaces;
};

struct AllocatedImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
};

struct AllocatedBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};

struct DeviceData {
    DeviceDispatch vt;
    InstanceData* inst = nullptr;
    VkPhysicalDevice physDev = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    Config config;

    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties memProps{};
    std::vector<VkQueueFamilyProperties> families;
    std::vector<uint32_t> appFamilies;  // unique queue families the application created queues from

    // The queue used for all of the layer's own work. When the driver has no
    // spare queue we share one with the application and serialise access with
    // a mutex (see queueMutex()).
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    uint32_t queueIndex = 0;
    bool queueShared = false;
    bool layerUsable = false;  // false: everything passes straight through

    VkCommandPool cmdPool = VK_NULL_HANDLE;
    bool hasSwapchainMaintenance1 = false;

    std::mutex queueMapMutex;
    std::unordered_map<VkQueue, std::unique_ptr<std::mutex>> queueMutexes;
    std::mutex& queueMutex(VkQueue q);

    std::mutex swapchainsMutex;
    std::unordered_map<VkSwapchainKHR, VirtualSwapchainPtr> swapchains;
    VirtualSwapchain* findSwapchain(VkSwapchainKHR sc);

    // Resource helpers (throw VkError on failure).
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags flags) const;
    AllocatedImage createImage(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage, bool withView,
                               VkImageCreateFlags flags = 0, const void* pNext = nullptr,
                               const std::vector<uint32_t>* sharingFamilies = nullptr);
    void destroyImage(AllocatedImage& img);
    AllocatedBuffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible = false);
    void destroyBuffer(AllocatedBuffer& buf);
    bool formatSupports(VkFormat format, VkFormatFeatureFlags features) const;

    // Allocates a primary command buffer from cmdPool with a valid dispatch pointer.
    VkResult allocateCommandBuffer(VkCommandBuffer* cmd);
    // Submit a command buffer on the layer queue (takes the queue mutex).
    VkResult submit(const VkSubmitInfo& info, VkFence fence);
    // Run a one-shot command buffer synchronously.
    template <typename F> void immediate(F&& record);
};

DeviceData* getDevice(void* key);
InstanceData* getInstance(void* key);

} // namespace bdex

namespace bdex {

template <typename F>
void DeviceData::immediate(F&& record) {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkResult r = allocateCommandBuffer(&cmd);
    if (r < 0) throw VkError(r, "vkAllocateCommandBuffers");
    try {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vt.BeginCommandBuffer(cmd, &bi));
        record(cmd);
        VK_CHECK(vt.EndCommandBuffer(cmd));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(vt.CreateFence(device, &fi, nullptr, &fence));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        VK_CHECK(submit(si, fence));
        VK_CHECK(vt.WaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    } catch (...) {
        if (fence) { vt.WaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX); vt.DestroyFence(device, fence, nullptr); }
        vt.FreeCommandBuffers(device, cmdPool, 1, &cmd);
        throw;
    }
    vt.DestroyFence(device, fence, nullptr);
    vt.FreeCommandBuffers(device, cmdPool, 1, &cmd);
}

} // namespace bdex
