#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>
#include <map>
#include <vector>

namespace bdex {

// Descriptor pool accounting for FrameGen: each descriptor set layout is
// registered with what one set of it holds, and allocations are planned
// against the current pool's remaining capacity; when a set does not fit, a
// new pool is opened (in chunks, covering every descriptor type seen so far).
// A pass therefore only declares its bindings and allocates its sets — there
// are no hand-maintained totals to keep in step with every optional pass (an
// undersized pool used to fail the creation and leave the swapchain in
// pass-through). Pure logic, unit-tested in tests/test_descriptor_planner.cpp;
// the Vulkan pools themselves are created by the caller.
class DescriptorPlanner {
public:
    using Counts = std::map<VkDescriptorType, uint32_t>;

    explicit DescriptorPlanner(uint32_t chunkSets = 64, uint32_t chunkPerType = 256)
        : chunkSets_(chunkSets), chunkPerType_(chunkPerType) {}

    // What one set of `layout` holds.
    void registerLayout(VkDescriptorSetLayout layout, const std::vector<VkDescriptorType>& bindings);
    // Plans one set of `layout`. Returns true when the caller must first
    // create a new pool of `poolSizes` / `maxSets` (the set then comes from
    // it); false when the current pool still has room.
    bool plan(VkDescriptorSetLayout layout, std::vector<VkDescriptorPoolSize>& poolSizes, uint32_t& maxSets);
    uint32_t poolCount() const { return pools_; }
    void reset() { *this = DescriptorPlanner(chunkSets_, chunkPerType_); }

private:
    uint32_t chunkSets_, chunkPerType_;
    std::map<VkDescriptorSetLayout, Counts> layouts_;
    Counts seen_;       // every type any registered layout uses
    Counts remaining_;  // in the current pool
    uint32_t setsLeft_ = 0;
    uint32_t pools_ = 0;
};

} // namespace bdex
