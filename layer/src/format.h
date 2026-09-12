#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>

namespace bdex {

// Packing performed by the interpolation shader. The shader writes raw bits
// into a mandatory-storage uint image which is then copied verbatim into the
// swapchain image, so the encoding must mirror the swapchain format's memory
// layout exactly.
enum : uint32_t { ENC_RGBA8 = 0, ENC_BGRA8 = 1, ENC_A2B10G10R10 = 2, ENC_A2R10G10B10 = 3, ENC_RGBA16F = 4, ENC_SRGB = 0x100 };

struct OutputEncoding {
    bool supported = false;
    VkFormat storageFormat = VK_FORMAT_UNDEFINED;  // R32_UINT or R32G32_UINT
    uint32_t encoding = 0;                          // ENC_* value (+ ENC_SRGB flag) for the shader
    bool is64 = false;
    bool srgb = false;
};

OutputEncoding encodingForFormat(VkFormat swapchainFormat);

} // namespace bdex
