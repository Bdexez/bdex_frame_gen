#include "format.h"

namespace bdex {

OutputEncoding encodingForFormat(VkFormat f) {
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
    return e;
}

} // namespace bdex
