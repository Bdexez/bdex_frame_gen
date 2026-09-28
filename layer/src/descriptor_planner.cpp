#include "descriptor_planner.h"

#include <algorithm>

namespace bdex {

void DescriptorPlanner::registerLayout(VkDescriptorSetLayout layout, const std::vector<VkDescriptorType>& bindings) {
    Counts c;
    for (VkDescriptorType t : bindings) {
        ++c[t];
        seen_[t];  // known type: new pools carry a chunk of it
    }
    layouts_[layout] = c;
}

bool DescriptorPlanner::plan(VkDescriptorSetLayout layout, std::vector<VkDescriptorPoolSize>& poolSizes,
                             uint32_t& maxSets) {
    const Counts& need = layouts_[layout];
    bool fits = setsLeft_ > 0;
    for (const auto& [type, n] : need)
        if (remaining_[type] < n) fits = false;
    bool fresh = false;
    if (!fits) {
        poolSizes.clear();
        remaining_.clear();
        for (const auto& kv : seen_) {
            auto it = need.find(kv.first);
            const uint32_t n = std::max(chunkPerType_, it == need.end() ? 0u : it->second);
            poolSizes.push_back({kv.first, n});
            remaining_[kv.first] = n;
        }
        maxSets = setsLeft_ = std::max(1u, chunkSets_);
        ++pools_;
        fresh = true;
    }
    --setsLeft_;
    for (const auto& [type, n] : need) remaining_[type] -= n;
    return fresh;
}

} // namespace bdex
