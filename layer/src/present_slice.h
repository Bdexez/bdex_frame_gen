#pragma once

#include "vk_compat.h"

#include <cstdint>
#include <vector>

namespace bdex {

// A vkQueuePresentKHR call for a subset of the original swapchains. Every
// supported per-swapchain pNext array is sliced in the same order as the
// swapchains. The source arrays must remain alive until the call completes.
class PresentSlice {
public:
    PresentSlice(const VkPresentInfoKHR& source, const std::vector<uint32_t>& positions, bool keepWaits);
    PresentSlice(const PresentSlice&) = delete;
    PresentSlice& operator=(const PresentSlice&) = delete;

    bool valid() const { return valid_; }
    const char* error() const { return error_; }
    const VkPresentInfoKHR& info() const { return info_; }
    const std::vector<VkResult>& results() const { return results_; }

private:
    VkPresentInfoKHR info_{};
    bool valid_ = false;
    const char* error_ = "";
    std::vector<VkSwapchainKHR> swapchains_;
    std::vector<uint32_t> indices_;
    std::vector<VkResult> results_;
    std::vector<uint32_t> deviceMasks_;
    std::vector<VkPresentRegionKHR> regions_;
    std::vector<uint64_t> ids_;
#ifdef VK_KHR_present_id2
    std::vector<uint64_t> ids2_;
#endif
    std::vector<VkFence> fences_;
    std::vector<VkPresentModeKHR> modes_;
#ifdef VK_GOOGLE_display_timing
    std::vector<VkPresentTimeGOOGLE> times_;
#endif
#ifdef VK_EXT_present_timing
    std::vector<VkPresentTimingInfoEXT> timings_;
#endif
    VkDeviceGroupPresentInfoKHR deviceGroup_{};
    VkPresentRegionsKHR presentRegions_{};
    VkPresentIdKHR presentId_{};
#ifdef VK_KHR_present_id2
    VkPresentId2KHR presentId2_{};
#endif
    VkSwapchainPresentFenceInfoEXT presentFences_{};
    VkSwapchainPresentModeInfoEXT presentModes_{};
#ifdef VK_GOOGLE_display_timing
    VkPresentTimesInfoGOOGLE presentTimes_{};
#endif
#ifdef VK_EXT_present_timing
    VkPresentTimingsInfoEXT presentTimings_{};
#endif
};

} // namespace bdex
