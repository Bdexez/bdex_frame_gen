#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>

namespace bdex {

// Packing performed by the interpolation shader. The shader writes raw bits
// into a mandatory-storage uint image which is then copied verbatim into the
// swapchain image, so the encoding must mirror the swapchain format's memory
// layout exactly.
enum : uint32_t { ENC_RGBA8 = 0, ENC_BGRA8 = 1, ENC_A2B10G10R10 = 2, ENC_A2R10G10B10 = 3, ENC_RGBA16F = 4, ENC_SRGB = 0x100 };

// Transfer function of the values the shaders sample from the swapchain
// images, which decides how they are brought to a perceptual scale where SDR
// white is about 1 (the scale the matching costs and thresholds are tuned
// on). Chosen from the format and the colour space.
enum : int32_t {
    TRANSFER_PERCEPTUAL = 0,  // gamma-encoded values (UNORM + sRGB non-linear, HLG, ...): used as they are
    TRANSFER_LINEAR = 1,      // linear with 1 = SDR white (*_SRGB formats decode on sampling, P3/BT.709 linear)
    TRANSFER_SCRGB = 2,       // scRGB / linear HDR: 1 = 80 nits, highlights far above 1 (FP16 swapchains)
    TRANSFER_PQ = 3,          // SMPTE ST 2084 (HDR10) encoded values: SDR white (203 nits) at ~0.58
};

struct OutputEncoding {
    bool supported = false;
    VkFormat storageFormat = VK_FORMAT_UNDEFINED;  // R32_UINT or R32G32_UINT
    uint32_t encoding = 0;                          // ENC_* value (+ ENC_SRGB flag) for the shader
    bool is64 = false;
    bool srgb = false;
    int32_t transfer = TRANSFER_PERCEPTUAL;  // TRANSFER_* for the shaders
};

OutputEncoding encodingForFormat(VkFormat swapchainFormat,
                                 VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);

} // namespace bdex
