#include "format.h"
#include "test.h"

using namespace bdex;

TEST(format_8bit_classes) {
    OutputEncoding e = encodingForFormat(VK_FORMAT_B8G8R8A8_UNORM);
    CHECK(e.supported && e.storageFormat == VK_FORMAT_R32_UINT && e.encoding == ENC_BGRA8 && !e.srgb && !e.is64);
    e = encodingForFormat(VK_FORMAT_B8G8R8A8_SRGB);
    CHECK(e.supported && e.encoding == (ENC_BGRA8 | ENC_SRGB) && e.srgb);
    e = encodingForFormat(VK_FORMAT_R8G8B8A8_SRGB);
    CHECK(e.supported && e.encoding == (ENC_RGBA8 | ENC_SRGB));
    e = encodingForFormat(VK_FORMAT_A8B8G8R8_UNORM_PACK32);
    CHECK(e.supported && e.encoding == ENC_RGBA8);
}

TEST(format_10bit_and_half) {
    OutputEncoding e = encodingForFormat(VK_FORMAT_A2B10G10R10_UNORM_PACK32);
    CHECK(e.supported && e.encoding == ENC_A2B10G10R10 && e.storageFormat == VK_FORMAT_R32_UINT);
    e = encodingForFormat(VK_FORMAT_A2R10G10B10_UNORM_PACK32);
    CHECK(e.supported && e.encoding == ENC_A2R10G10B10);
    e = encodingForFormat(VK_FORMAT_R16G16B16A16_SFLOAT);
    CHECK(e.supported && e.is64 && e.storageFormat == VK_FORMAT_R32G32_UINT && e.encoding == ENC_RGBA16F);
}

TEST(format_unsupported) {
    CHECK(!encodingForFormat(VK_FORMAT_R5G6B5_UNORM_PACK16).supported);
    CHECK(!encodingForFormat(VK_FORMAT_R32G32B32A32_SFLOAT).supported);
    CHECK(!encodingForFormat(VK_FORMAT_UNDEFINED).supported);
}
