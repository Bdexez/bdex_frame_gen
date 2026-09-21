#include "present_slice.h"

namespace bdex {

namespace {

template <class T>
bool take(std::vector<T>& dst, const T* src, uint32_t count, uint32_t expected,
          const std::vector<uint32_t>& positions) {
    if (count != expected || (count && !src)) return false;
    dst.reserve(positions.size());
    for (uint32_t i : positions) dst.push_back(src[i]);
    return true;
}

} // namespace

PresentSlice::PresentSlice(const VkPresentInfoKHR& source, const std::vector<uint32_t>& positions, bool keepWaits) {
    info_.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    if (positions.empty() || !source.pSwapchains || !source.pImageIndices) {
        error_ = "empty or incomplete present";
        return;
    }
    for (uint32_t i : positions) {
        if (i >= source.swapchainCount) {
            error_ = "swapchain index outside present";
            return;
        }
        swapchains_.push_back(source.pSwapchains[i]);
        indices_.push_back(source.pImageIndices[i]);
    }
    if (source.pResults) results_.resize(positions.size(), VK_SUCCESS);
    info_.waitSemaphoreCount = keepWaits ? source.waitSemaphoreCount : 0;
    info_.pWaitSemaphores = keepWaits ? source.pWaitSemaphores : nullptr;
    info_.swapchainCount = static_cast<uint32_t>(positions.size());
    info_.pSwapchains = swapchains_.data();
    info_.pImageIndices = indices_.data();
    info_.pResults = results_.empty() ? nullptr : results_.data();

    const void** tail = &info_.pNext;
    for (auto* base = static_cast<const VkBaseInStructure*>(source.pNext); base; base = base->pNext) {
        switch (base->sType) {
        case VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR: {
            auto& src = *reinterpret_cast<const VkDeviceGroupPresentInfoKHR*>(base);
            if (!take(deviceMasks_, src.pDeviceMasks, src.swapchainCount, source.swapchainCount, positions)) {
                error_ = "invalid device-group present array"; return;
            }
            deviceGroup_ = src;
            deviceGroup_.swapchainCount = info_.swapchainCount;
            deviceGroup_.pDeviceMasks = deviceMasks_.data();
            deviceGroup_.pNext = nullptr;
            *tail = &deviceGroup_; tail = &deviceGroup_.pNext;
            break;
        }
        case VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR: {
            auto& src = *reinterpret_cast<const VkPresentRegionsKHR*>(base);
            if (!take(regions_, src.pRegions, src.swapchainCount, source.swapchainCount, positions)) {
                error_ = "invalid present-region array"; return;
            }
            presentRegions_ = src;
            presentRegions_.swapchainCount = info_.swapchainCount;
            presentRegions_.pRegions = regions_.data();
            presentRegions_.pNext = nullptr;
            *tail = &presentRegions_; tail = &presentRegions_.pNext;
            break;
        }
        case VK_STRUCTURE_TYPE_PRESENT_ID_KHR: {
            auto& src = *reinterpret_cast<const VkPresentIdKHR*>(base);
            if (!take(ids_, src.pPresentIds, src.swapchainCount, source.swapchainCount, positions)) {
                error_ = "invalid present-ID array"; return;
            }
            presentId_ = src;
            presentId_.swapchainCount = info_.swapchainCount;
            presentId_.pPresentIds = ids_.data();
            presentId_.pNext = nullptr;
            *tail = &presentId_; tail = &presentId_.pNext;
            break;
        }
#ifdef VK_KHR_present_id2
        case VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR: {
            auto& src = *reinterpret_cast<const VkPresentId2KHR*>(base);
            if (!take(ids2_, src.pPresentIds, src.swapchainCount, source.swapchainCount, positions)) {
                error_ = "invalid present-ID2 array"; return;
            }
            presentId2_ = src;
            presentId2_.swapchainCount = info_.swapchainCount;
            presentId2_.pPresentIds = ids2_.data();
            presentId2_.pNext = nullptr;
            *tail = &presentId2_; tail = &presentId2_.pNext;
            break;
        }
#endif
        case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT: {
            auto& src = *reinterpret_cast<const VkSwapchainPresentFenceInfoEXT*>(base);
            if (!take(fences_, src.pFences, src.swapchainCount, source.swapchainCount, positions)) {
                error_ = "invalid present-fence array"; return;
            }
            presentFences_ = src;
            presentFences_.swapchainCount = info_.swapchainCount;
            presentFences_.pFences = fences_.data();
            presentFences_.pNext = nullptr;
            *tail = &presentFences_; tail = &presentFences_.pNext;
            break;
        }
        case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT: {
            auto& src = *reinterpret_cast<const VkSwapchainPresentModeInfoEXT*>(base);
            if (!take(modes_, src.pPresentModes, src.swapchainCount, source.swapchainCount, positions)) {
                error_ = "invalid present-mode array"; return;
            }
            presentModes_ = src;
            presentModes_.swapchainCount = info_.swapchainCount;
            presentModes_.pPresentModes = modes_.data();
            presentModes_.pNext = nullptr;
            *tail = &presentModes_; tail = &presentModes_.pNext;
            break;
        }
#ifdef VK_GOOGLE_display_timing
        case VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE: {
            auto& src = *reinterpret_cast<const VkPresentTimesInfoGOOGLE*>(base);
            if (!take(times_, src.pTimes, src.swapchainCount, source.swapchainCount, positions)) {
                error_ = "invalid present-time array"; return;
            }
            presentTimes_ = src;
            presentTimes_.swapchainCount = info_.swapchainCount;
            presentTimes_.pTimes = times_.data();
            presentTimes_.pNext = nullptr;
            *tail = &presentTimes_; tail = &presentTimes_.pNext;
            break;
        }
#endif
#ifdef VK_EXT_present_timing
        case VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT: {
            auto& src = *reinterpret_cast<const VkPresentTimingsInfoEXT*>(base);
            if (!take(timings_, src.pTimingInfos, src.swapchainCount, source.swapchainCount, positions)) {
                error_ = "invalid present-timing array"; return;
            }
            presentTimings_ = src;
            presentTimings_.swapchainCount = info_.swapchainCount;
            presentTimings_.pTimingInfos = timings_.data();
            presentTimings_.pNext = nullptr;
            *tail = &presentTimings_; tail = &presentTimings_.pNext;
            break;
        }
#endif
        default:
            // We cannot safely forward a structure with an unknown number of
            // per-swapchain entries after changing swapchainCount.
            error_ = "unsupported multi-swapchain present pNext";
            return;
        }
    }
    valid_ = true;
}

} // namespace bdex
