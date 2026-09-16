#pragma once
// Dispatch tables holding the next-layer / driver entry points.
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

namespace bdex {

#define BDEX_INSTANCE_FUNCS(X)                       \
    X(GetInstanceProcAddr)                           \
    X(DestroyInstance)                               \
    X(EnumerateDeviceExtensionProperties)            \
    X(EnumeratePhysicalDevices)                      \
    X(GetPhysicalDeviceProperties)                   \
    X(GetPhysicalDeviceQueueFamilyProperties)        \
    X(GetPhysicalDeviceMemoryProperties)             \
    X(GetPhysicalDeviceFormatProperties)             \
    X(GetPhysicalDeviceFeatures)                     \
    X(GetPhysicalDeviceSurfaceSupportKHR)            \
    X(GetPhysicalDeviceSurfaceCapabilitiesKHR)       \
    X(GetPhysicalDeviceSurfaceCapabilities2KHR)      \
    X(GetPhysicalDeviceSurfacePresentModesKHR)

#define BDEX_DEVICE_FUNCS(X)          \
    X(GetDeviceProcAddr)              \
    X(DestroyDevice)                  \
    X(GetDeviceQueue)                 \
    X(GetDeviceQueue2)                \
    X(QueueSubmit)                    \
    X(QueueSubmit2)                   \
    X(QueueSubmit2KHR)                \
    X(QueueWaitIdle)                  \
    X(QueueBindSparse)                \
    X(DeviceWaitIdle)                 \
    X(CreateSwapchainKHR)             \
    X(DestroySwapchainKHR)            \
    X(GetSwapchainImagesKHR)          \
    X(AcquireNextImageKHR)            \
    X(AcquireNextImage2KHR)           \
    X(QueuePresentKHR)                \
    X(WaitForPresentKHR)              \
    X(GetSwapchainStatusKHR)          \
    X(ReleaseSwapchainImagesEXT)      \
    X(CreateImage)                    \
    X(DestroyImage)                   \
    X(CreateImageView)                \
    X(DestroyImageView)               \
    X(GetImageMemoryRequirements)     \
    X(AllocateMemory)                 \
    X(FreeMemory)                     \
    X(BindImageMemory)                \
    X(CreateBuffer)                   \
    X(DestroyBuffer)                  \
    X(GetBufferMemoryRequirements)    \
    X(BindBufferMemory)               \
    X(MapMemory)                      \
    X(UnmapMemory)                    \
    X(CmdCopyImageToBuffer)           \
    X(CmdCopyBuffer)                  \
    X(CreateSampler)                  \
    X(DestroySampler)                 \
    X(CreateShaderModule)             \
    X(DestroyShaderModule)            \
    X(CreateDescriptorSetLayout)      \
    X(DestroyDescriptorSetLayout)     \
    X(CreatePipelineLayout)           \
    X(DestroyPipelineLayout)          \
    X(CreateComputePipelines)         \
    X(DestroyPipeline)                \
    X(CreateDescriptorPool)           \
    X(DestroyDescriptorPool)          \
    X(AllocateDescriptorSets)         \
    X(UpdateDescriptorSets)           \
    X(CreateCommandPool)              \
    X(DestroyCommandPool)             \
    X(AllocateCommandBuffers)         \
    X(FreeCommandBuffers)             \
    X(BeginCommandBuffer)             \
    X(EndCommandBuffer)               \
    X(ResetCommandBuffer)             \
    X(CreateFence)                    \
    X(DestroyFence)                   \
    X(WaitForFences)                  \
    X(ResetFences)                    \
    X(GetFenceStatus)                 \
    X(CreateSemaphore)                \
    X(DestroySemaphore)               \
    X(CmdPipelineBarrier)             \
    X(CmdCopyImage)                   \
    X(CmdBindPipeline)                \
    X(CmdBindDescriptorSets)          \
    X(CmdPushConstants)               \
    X(CmdDispatch)                    \
    X(CmdFillBuffer)                  \
    X(CreateQueryPool)                \
    X(DestroyQueryPool)               \
    X(CmdResetQueryPool)              \
    X(CmdWriteTimestamp)              \
    X(GetQueryPoolResults)

struct InstanceDispatch {
#define X(name) PFN_vk##name name = nullptr;
    BDEX_INSTANCE_FUNCS(X)
#undef X
    void load(VkInstance instance, PFN_vkGetInstanceProcAddr gpa) {
        GetInstanceProcAddr = gpa;
#define X(name) name = reinterpret_cast<PFN_vk##name>(gpa(instance, "vk" #name));
        BDEX_INSTANCE_FUNCS(X)
#undef X
    }
};

struct DeviceDispatch {
#define X(name) PFN_vk##name name = nullptr;
    BDEX_DEVICE_FUNCS(X)
#undef X
    void load(VkDevice device, PFN_vkGetDeviceProcAddr gdpa) {
        GetDeviceProcAddr = gdpa;
#define X(name) name = reinterpret_cast<PFN_vk##name>(gdpa(device, "vk" #name));
        BDEX_DEVICE_FUNCS(X)
#undef X
    }
};

// Every dispatchable handle starts with a pointer to the loader's dispatch
// table; it is unique per instance / device and shared by their children.
inline void* dispatchKey(const void* handle) { return *static_cast<void* const*>(handle); }

// Dispatchable objects the layer creates itself (queues, command buffers)
// bypass the loader's trampolines, so the loader never fills in their
// dispatch pointer; layers below us (validation...) key on it. Copy the
// parent device's.
inline void adoptDispatch(void* handle, const void* device) { *static_cast<void**>(handle) = dispatchKey(device); }

} // namespace bdex
