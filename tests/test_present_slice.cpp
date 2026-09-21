#include "present_slice.h"
#include "test.h"

namespace {
template <class T>
const T* presentNode(const void* chain, VkStructureType type) {
    for (auto* node = static_cast<const VkBaseInStructure*>(chain); node; node = node->pNext)
        if (node->sType == type) return reinterpret_cast<const T*>(node);
    return nullptr;
}
} // namespace

TEST(present_slice_reindexes_two_swapchains_and_metadata) {
    VkSwapchainKHR swapchains[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    uint32_t images[2] = {7, 19};
    VkResult results[2] = {VK_SUCCESS, VK_SUCCESS};
    VkSemaphore wait = VK_NULL_HANDLE;
    uint64_t ids[2] = {101, 202};
    VkPresentModeKHR modes[2] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR};
    VkFence fences[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    uint32_t masks[2] = {1, 3};
    VkRectLayerKHR rects[2]{};
    VkPresentRegionKHR regions[2] = {{1, &rects[0]}, {2, rects}};

    VkDeviceGroupPresentInfoKHR group{};
    group.sType = VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR;
    group.swapchainCount = 2;
    group.pDeviceMasks = masks;
    group.mode = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
    VkSwapchainPresentFenceInfoEXT fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT;
    fenceInfo.pNext = &group;
    fenceInfo.swapchainCount = 2;
    fenceInfo.pFences = fences;
    VkPresentRegionsKHR regionInfo{};
    regionInfo.sType = VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR;
    regionInfo.pNext = &fenceInfo;
    regionInfo.swapchainCount = 2;
    regionInfo.pRegions = regions;
    VkSwapchainPresentModeInfoEXT modeInfo{};
    modeInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT;
    modeInfo.pNext = &regionInfo;
    modeInfo.swapchainCount = 2;
    modeInfo.pPresentModes = modes;
    VkPresentIdKHR idInfo{};
    idInfo.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
    idInfo.pNext = &modeInfo;
    idInfo.swapchainCount = 2;
    idInfo.pPresentIds = ids;
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.pNext = &idInfo;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &wait;
    present.swapchainCount = 2;
    present.pSwapchains = swapchains;
    present.pImageIndices = images;
    present.pResults = results;

    // The pass-through swapchain may be second in the application's array.
    bdex::PresentSlice real(present, {1}, true);
    CHECK(real.valid());
    CHECK(real.info().swapchainCount == 1);
    CHECK(real.info().pImageIndices[0] == 19);
    CHECK(real.info().waitSemaphoreCount == 1);
    CHECK(real.info().pWaitSemaphores == &wait);
    auto* realId = presentNode<VkPresentIdKHR>(real.info().pNext, VK_STRUCTURE_TYPE_PRESENT_ID_KHR);
    auto* realMode = presentNode<VkSwapchainPresentModeInfoEXT>(real.info().pNext, VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT);
    auto* realRegions = presentNode<VkPresentRegionsKHR>(real.info().pNext, VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR);
    auto* realFences = presentNode<VkSwapchainPresentFenceInfoEXT>(real.info().pNext, VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT);
    auto* realGroup = presentNode<VkDeviceGroupPresentInfoKHR>(real.info().pNext, VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR);
    CHECK(realId && realId->swapchainCount == 1 && realId->pPresentIds[0] == 202);
    CHECK(realMode && realMode->swapchainCount == 1 && realMode->pPresentModes[0] == VK_PRESENT_MODE_MAILBOX_KHR);
    CHECK(realRegions && realRegions->swapchainCount == 1 && realRegions->pRegions[0].rectangleCount == 2);
    CHECK(realFences && realFences->swapchainCount == 1 && realFences->pFences[0] == fences[1]);
    CHECK(realGroup && realGroup->swapchainCount == 1 && realGroup->pDeviceMasks[0] == 3);
    real.info().pResults[0] = VK_SUBOPTIMAL_KHR;
    CHECK(real.results()[0] == VK_SUBOPTIMAL_KHR);
    CHECK(results[1] == VK_SUCCESS);  // the caller copies the subset result back

    bdex::PresentSlice virt(present, {0}, false);
    CHECK(virt.valid());
    CHECK(virt.info().pImageIndices[0] == 7);
    CHECK(virt.info().waitSemaphoreCount == 0);
    CHECK(virt.info().pWaitSemaphores == nullptr);
    auto* virtualId = presentNode<VkPresentIdKHR>(virt.info().pNext, VK_STRUCTURE_TYPE_PRESENT_ID_KHR);
    CHECK(virtualId && virtualId->pPresentIds[0] == 101);
}

TEST(present_slice_rejects_unmappable_metadata) {
    VkSwapchainKHR swaps[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    uint32_t images[2] = {0, 1};
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.swapchainCount = 2;
    present.pSwapchains = swaps;
    present.pImageIndices = images;

    VkPresentIdKHR ids{};
    ids.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
    uint64_t onlyOne = 1;
    ids.swapchainCount = 1;
    ids.pPresentIds = &onlyOne;
    present.pNext = &ids;
    bdex::PresentSlice mismatched(present, {1}, true);
    CHECK(!mismatched.valid());

    VkBaseInStructure unknown{static_cast<VkStructureType>(0x7fffffff), nullptr};
    present.pNext = &unknown;
    bdex::PresentSlice unsupported(present, {1}, true);
    CHECK(!unsupported.valid());
}
