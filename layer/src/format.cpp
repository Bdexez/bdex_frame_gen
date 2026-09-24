#include "format.h"

namespace bdex {

OutputEncoding encodingForFormat(VkFormat f, VkColorSpaceKHR cs) {
    OutputEncoding e;
    e.supported = true;
    e.storageFormat = VK_FORMAT_R32_UINT;
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM:            e.encoding = ENC_RGBA8; break;
    case VK_FORMAT_R8G8B8A8_SRGB:             e.encoding = ENC_RGBA8; e.srgb = true; break;
    case VK_FORMAT_B8G8R8A8_UNORM:            e.encoding = ENC_BGRA8; break;
    case VK_FORMAT_B8G8R8A8_SRGB:             e.encoding = ENC_BGRA8; e.srgb = true; break;
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:     e.encoding = ENC_RGBA8; break;
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:      e.encoding = ENC_RGBA8; e.srgb = true; break;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:  e.encoding = ENC_A2B10G10R10; break;
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:  e.encoding = ENC_A2R10G10B10; break;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        e.encoding = ENC_RGBA16F;
        e.storageFormat = VK_FORMAT_R32G32_UINT;
        e.is64 = true;
        break;
    default:
        e.supported = false;
        e.storageFormat = VK_FORMAT_UNDEFINED;
        break;
    }
    if (e.srgb) e.encoding |= ENC_SRGB;
    // The matching works on a perceptual scale with SDR white near 1. sRGB
    // formats are decoded to linear on sampling; HDR colour spaces put white
    // elsewhere entirely (scRGB: 1 = 80 nits, so a 1000-nit highlight is
    // 12.5; PQ: white at 0.58), which would throw the matching costs and the
    // scene-cut thresholds off by an order of magnitude.
    if (e.srgb) e.transfer = TRANSFER_LINEAR;
    else if (cs == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT || cs == VK_COLOR_SPACE_BT2020_LINEAR_EXT) e.transfer = TRANSFER_SCRGB;
    else if (cs == VK_COLOR_SPACE_HDR10_ST2084_EXT) e.transfer = TRANSFER_PQ;
    else if (cs == VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT || cs == VK_COLOR_SPACE_BT709_LINEAR_EXT) e.transfer = TRANSFER_LINEAR;
    return e;
}

} // namespace bdex
