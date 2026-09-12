#pragma once
#include <vulkan/vulkan.h>

#include <stdexcept>
#include <string>

namespace bdex {

const char* vkResultName(VkResult r);

struct VkError : std::runtime_error {
    VkResult result;
    VkError(VkResult r, const char* what) : std::runtime_error(std::string(what) + ": " + vkResultName(r)), result(r) {}
};

#define VK_CHECK(expr)                                                  \
    do {                                                                \
        VkResult _r = (expr);                                           \
        if (_r < 0) throw ::bdex::VkError(_r, #expr);                   \
    } while (0)

inline VkImageSubresourceRange colorRange() {
    return VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
}

inline VkImageMemoryBarrier imageBarrier(VkImage image, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                                         VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = colorRange();
    return b;
}

// Walk a pNext chain looking for a structure type.
template <typename T>
const T* findChain(const void* pNext, VkStructureType type) {
    const VkBaseInStructure* p = static_cast<const VkBaseInStructure*>(pNext);
    while (p) {
        if (p->sType == type) return reinterpret_cast<const T*>(p);
        p = p->pNext;
    }
    return nullptr;
}

} // namespace bdex
