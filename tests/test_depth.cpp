// The depth option's choice of the game's depth buffer (depth::choose) and
// how it is read (depth::readLayout): depth_select.cpp, no GPU.
#include "depth_tracker.h"
#include "test.h"

#include <vector>

namespace depth = bdex::depth;

namespace {
depth::Usage use(uint64_t image, uint32_t w, uint32_t h, uint32_t passes, bool sampleable = true) {
    depth::Usage u;
    u.image = image;
    u.extent = {w, h};
    u.area = {{0, 0}, {w, h}};
    u.passes = passes;
    u.sampleable = sampleable;
    return u;
}
const VkExtent2D k1440{2560, 1440};
} // namespace

TEST(depth_choose_the_scene_buffer_not_a_shadow_map) {
    // A square shadow map drawn into more often, and the scene's depth.
    std::vector<depth::Usage> used{use(1, 4096, 4096, 40), use(2, 2560, 1440, 6)};
    CHECK(depth::choose(used, k1440, 0) == 1);
}

TEST(depth_choose_a_render_resolution_and_not_a_small_buffer) {
    // DLSS / FSR at 33 %: 853x480 still counts; a 320x180 one (a reflection,
    // a minimap) does not.
    std::vector<depth::Usage> used{use(1, 853, 480, 5)};
    CHECK(depth::choose(used, k1440, 0) == 0);
    used = {use(1, 320, 180, 50)};
    CHECK(depth::choose(used, k1440, 0) == -1);
}

TEST(depth_choose_the_most_used_and_keep_the_previous_pick_when_close) {
    std::vector<depth::Usage> used{use(1, 2560, 1440, 10), use(2, 2560, 1440, 14)};
    CHECK(depth::choose(used, k1440, 0) == 1);
    CHECK(depth::choose(used, k1440, 1) == 0);   // 10 against 14: within half, kept
    used[1].passes = 30;
    CHECK(depth::choose(used, k1440, 1) == 1);   // 10 against 30: the other takes over
}

TEST(depth_choose_needs_a_readable_buffer_drawn_this_frame) {
    std::vector<depth::Usage> used{use(1, 2560, 1440, 8, false), use(2, 2560, 1440, 0)};
    CHECK(depth::choose(used, k1440, 0) == -1);
    CHECK(depth::choose({}, k1440, 0) == -1);
    CHECK(depth::choose(used, {0, 0}, 0) == -1);
}

TEST(depth_choose_follows_the_area_drawn_not_the_image_size) {
    // A dynamic resolution drawing into part of a larger image: the area's
    // proportions count.
    depth::Usage u = use(1, 4096, 4096, 6);
    u.area = {{0, 0}, {1920, 1080}};
    CHECK(depth::choose({u}, k1440, 0) == 0);
}

TEST(depth_read_layouts) {
    VkImageLayout r = VK_IMAGE_LAYOUT_UNDEFINED;
    CHECK(depth::readLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, r) &&
          r == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    CHECK(depth::readLayout(VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, r) && r == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
    CHECK(depth::readLayout(VK_IMAGE_LAYOUT_GENERAL, r) && r == VK_IMAGE_LAYOUT_GENERAL);
    CHECK(depth::readLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, r) && r == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK(!depth::readLayout(VK_IMAGE_LAYOUT_UNDEFINED, r));
    CHECK(!depth::readLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, r));
}

TEST(depth_formats) {
    CHECK(depth::isDepthFormat(VK_FORMAT_D24_UNORM_S8_UINT) && depth::hasStencil(VK_FORMAT_D24_UNORM_S8_UINT));
    CHECK(depth::isDepthFormat(VK_FORMAT_D32_SFLOAT) && !depth::hasStencil(VK_FORMAT_D32_SFLOAT));
    CHECK(!depth::isDepthFormat(VK_FORMAT_R32_SFLOAT) && !depth::isDepthFormat(VK_FORMAT_S8_UINT));
}
