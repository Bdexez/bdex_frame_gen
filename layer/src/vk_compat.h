#pragma once
// The Vulkan headers, and what older ones lack. The layer builds against a
// distribution's own headers: Ubuntu 22.04 ships 1.3.204, which predates
// VK_EXT_surface_maintenance1 / VK_EXT_swapchain_maintenance1 (1.3.237); their
// definitions below are copied from the Khronos headers (1.3.260, Apache-2.0
// OR MIT) and only compiled when the system's headers lack them. Debian 12's
// 1.3.239 has them, with a non-const pNext on two present structures
// (bdexNext).
#include <vulkan/vulkan.h>

#ifndef VK_EXT_surface_maintenance1
#define VK_EXT_surface_maintenance1 1
#define VK_EXT_SURFACE_MAINTENANCE_1_SPEC_VERSION 1
#define VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME "VK_EXT_surface_maintenance1"
typedef enum VkPresentScalingFlagBitsEXT {
    VK_PRESENT_SCALING_ONE_TO_ONE_BIT_EXT = 0x00000001,
    VK_PRESENT_SCALING_ASPECT_RATIO_STRETCH_BIT_EXT = 0x00000002,
    VK_PRESENT_SCALING_STRETCH_BIT_EXT = 0x00000004,
    VK_PRESENT_SCALING_FLAG_BITS_MAX_ENUM_EXT = 0x7FFFFFFF
} VkPresentScalingFlagBitsEXT;
typedef VkFlags VkPresentScalingFlagsEXT;
typedef enum VkPresentGravityFlagBitsEXT {
    VK_PRESENT_GRAVITY_MIN_BIT_EXT = 0x00000001,
    VK_PRESENT_GRAVITY_MAX_BIT_EXT = 0x00000002,
    VK_PRESENT_GRAVITY_CENTERED_BIT_EXT = 0x00000004,
    VK_PRESENT_GRAVITY_FLAG_BITS_MAX_ENUM_EXT = 0x7FFFFFFF
} VkPresentGravityFlagBitsEXT;
typedef VkFlags VkPresentGravityFlagsEXT;
typedef struct VkSurfacePresentModeEXT {
    VkStructureType     sType;
    void*               pNext;
    VkPresentModeKHR    presentMode;
} VkSurfacePresentModeEXT;
typedef struct VkSurfacePresentScalingCapabilitiesEXT {
    VkStructureType             sType;
    void*                       pNext;
    VkPresentScalingFlagsEXT    supportedPresentScaling;
    VkPresentGravityFlagsEXT    supportedPresentGravityX;
    VkPresentGravityFlagsEXT    supportedPresentGravityY;
    VkExtent2D                  minScaledImageExtent;
    VkExtent2D                  maxScaledImageExtent;
} VkSurfacePresentScalingCapabilitiesEXT;
typedef struct VkSurfacePresentModeCompatibilityEXT {
    VkStructureType      sType;
    void*                pNext;
    uint32_t             presentModeCount;
    VkPresentModeKHR*    pPresentModes;
} VkSurfacePresentModeCompatibilityEXT;
// (VkStructureType members in newer headers.)
constexpr VkStructureType VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_EXT = VkStructureType(1000274000);
constexpr VkStructureType VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_EXT = VkStructureType(1000274001);
constexpr VkStructureType VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_EXT = VkStructureType(1000274002);
#endif

#ifndef VK_EXT_swapchain_maintenance1
#define VK_EXT_swapchain_maintenance1 1
#define VK_EXT_SWAPCHAIN_MAINTENANCE_1_SPEC_VERSION 1
#define VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME "VK_EXT_swapchain_maintenance1"
typedef struct VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT {
    VkStructureType    sType;
    void*              pNext;
    VkBool32           swapchainMaintenance1;
} VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT;
typedef struct VkSwapchainPresentFenceInfoEXT {
    VkStructureType    sType;
    const void*        pNext;
    uint32_t           swapchainCount;
    const VkFence*     pFences;
} VkSwapchainPresentFenceInfoEXT;
typedef struct VkSwapchainPresentModesCreateInfoEXT {
    VkStructureType            sType;
    const void*                pNext;
    uint32_t                   presentModeCount;
    const VkPresentModeKHR*    pPresentModes;
} VkSwapchainPresentModesCreateInfoEXT;
typedef struct VkSwapchainPresentModeInfoEXT {
    VkStructureType            sType;
    const void*                pNext;
    uint32_t                   swapchainCount;
    const VkPresentModeKHR*    pPresentModes;
} VkSwapchainPresentModeInfoEXT;
typedef struct VkSwapchainPresentScalingCreateInfoEXT {
    VkStructureType             sType;
    const void*                 pNext;
    VkPresentScalingFlagsEXT    scalingBehavior;
    VkPresentGravityFlagsEXT    presentGravityX;
    VkPresentGravityFlagsEXT    presentGravityY;
} VkSwapchainPresentScalingCreateInfoEXT;
typedef struct VkReleaseSwapchainImagesInfoEXT {
    VkStructureType    sType;
    const void*        pNext;
    VkSwapchainKHR     swapchain;
    uint32_t           imageIndexCount;
    const uint32_t*    pImageIndices;
} VkReleaseSwapchainImagesInfoEXT;
typedef VkResult (VKAPI_PTR *PFN_vkReleaseSwapchainImagesEXT)(VkDevice device, const VkReleaseSwapchainImagesInfoEXT* pReleaseInfo);
constexpr VkStructureType VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT = VkStructureType(1000275000);
constexpr VkStructureType VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT = VkStructureType(1000275001);
constexpr VkStructureType VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT = VkStructureType(1000275002);
constexpr VkStructureType VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT = VkStructureType(1000275003);
constexpr VkStructureType VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_EXT = VkStructureType(1000275004);
constexpr VkStructureType VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_EXT = VkStructureType(1000275005);
constexpr VkSwapchainCreateFlagBitsKHR VK_SWAPCHAIN_CREATE_DEFERRED_MEMORY_ALLOCATION_BIT_EXT =
    VkSwapchainCreateFlagBitsKHR(0x00000008);
#endif

// The next-pointer slot of a structure being chained, as `const void**`:
// headers before 1.3.241 declare some present structures' pNext non-const.
template <typename T>
inline const void** bdexNext(T& s) {
    return static_cast<const void**>(static_cast<void*>(&s.pNext));
}
