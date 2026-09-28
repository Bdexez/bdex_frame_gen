#include "descriptor_planner.h"
#include "test.h"

using namespace bdex;

namespace {
// Non-dispatchable handles are pointers on 64-bit targets and uint64_t on
// 32-bit ones: a C-style cast converts to either.
VkDescriptorSetLayout fakeLayout(uintptr_t v) { return (VkDescriptorSetLayout)(v); }
const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
const VkDescriptorType SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
const VkDescriptorType SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
}

TEST(descriptor_planner_opens_pools_as_needed) {
    DescriptorPlanner p(4, 8);
    const auto a = fakeLayout(0x10), b = fakeLayout(0x20);
    p.registerLayout(a, {CIS, CIS, SI});
    p.registerLayout(b, {SB});
    std::vector<VkDescriptorPoolSize> sizes;
    uint32_t sets = 0;
    CHECK(p.plan(a, sizes, sets));  // the first set opens a pool
    CHECK(sets == 4 && sizes.size() == 3);  // every type seen, one chunk each
    CHECK(!p.plan(a, sizes, sets));
    CHECK(!p.plan(b, sizes, sets));
    CHECK(!p.plan(a, sizes, sets));  // 4 sets: 6 CIS of 8, 3 SI
    CHECK(p.plan(b, sizes, sets));   // out of sets
    CHECK(p.poolCount() == 2);
}

TEST(descriptor_planner_counts_every_type) {
    DescriptorPlanner p(100, 4);
    const auto a = fakeLayout(0x10);
    p.registerLayout(a, {CIS, CIS, CIS});
    std::vector<VkDescriptorPoolSize> sizes;
    uint32_t sets = 0;
    CHECK(p.plan(a, sizes, sets));
    CHECK(p.plan(a, sizes, sets));  // 6 > 4 samplers: a new pool
    CHECK(p.poolCount() == 2);
}

TEST(descriptor_planner_sizes_a_pool_for_a_large_set) {
    DescriptorPlanner p(8, 2);
    const auto a = fakeLayout(0x10);
    p.registerLayout(a, {SI, SI, SI, SI, SI});
    std::vector<VkDescriptorPoolSize> sizes;
    uint32_t sets = 0;
    CHECK(p.plan(a, sizes, sets));
    CHECK(sizes.size() == 1 && sizes[0].type == SI && sizes[0].descriptorCount == 5);
}
