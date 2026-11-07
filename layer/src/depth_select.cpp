// The depth option's pure decisions (depth_tracker.h): which depth image a
// frame's scene was drawn with, and how to read it. No Vulkan device: unit
// tested (tests/test_depth.cpp).
#include "depth_tracker.h"

#include <cmath>

namespace bdex {

namespace depth {

bool isDepthFormat(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return true;
    default:
        return false;
    }
}

bool hasStencil(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

int choose(const std::vector<Usage>& used, VkExtent2D target, uint64_t previous) {
    if (target.width == 0 || target.height == 0) return -1;
    const double aspect = double(target.width) / double(target.height);
    int best = -1, prev = -1;
    double bestScore = 0.0, prevScore = 0.0;
    for (size_t i = 0; i < used.size(); ++i) {
        const Usage& u = used[i];
        if (!u.sampleable || u.passes == 0 || u.area.extent.width == 0 || u.area.extent.height == 0) continue;
        const double a = double(u.area.extent.width) / double(u.area.extent.height);
        if (std::abs(a / aspect - 1.0) > 0.1) continue;            // not the picture's proportions
        if (u.area.extent.width * 10 < target.width * 3) continue;  // under 30 % of its width
        const double score = double(u.passes) * double(u.area.extent.width) * double(u.area.extent.height);
        if (score > bestScore) { bestScore = score; best = int(i); }
        if (u.image == previous) { prevScore = score; prev = int(i); }
    }
    if (prev >= 0 && prevScore * 2.0 >= bestScore) return prev;  // (no flip-flop between close ones)
    return best;
}

bool readLayout(VkImageLayout current, VkImageLayout& read) {
    switch (current) {
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        read = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        return true;
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
        read = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        return true;
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_GENERAL:
        read = current;
        return true;
    default:
        return false;  // UNDEFINED (unknown), a transfer layout...
    }
}

} // namespace depth

} // namespace bdex
