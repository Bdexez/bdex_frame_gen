// Exhaust a virtual swapchain and check vkAcquireNextImageKHR timeout results.
// This is a Vulkan integration test, run with the layer enabled under Xvfb.
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

static void check(VkResult result, const char* call) {
    if (result < 0) throw std::runtime_error(std::string(call) + ": " + std::to_string(result));
}

int main() {
    if (!glfwInit()) {
        const char* required = std::getenv("BDEX_REQUIRE_DISPLAY");
        if (required && std::string(required) == "1") {
            std::fprintf(stderr, "glfwInit failed with BDEX_REQUIRE_DISPLAY set\n");
            return 1;
        }
        std::puts("no usable display; skipping acquire integration test");
        return 77;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(640, 480, "bdex acquire test", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }

    std::string logPath;
    try {
        char logTemplate[] = "/tmp/bdex-acquire-XXXXXX";
        int logFd = mkstemp(logTemplate);
        if (logFd < 0) throw std::runtime_error("could not create layer log");
        close(logFd);
        logPath = logTemplate;
        unsetenv("VK_ADD_IMPLICIT_LAYER_PATH");
        if (setenv("BDEX_FG_LOG_FILE", logPath.c_str(), 1) != 0)
            throw std::runtime_error("could not set layer log path");

        VkInstance instance = VK_NULL_HANDLE;
        uint32_t extensionCount = 0;
        const char** extensions = glfwGetRequiredInstanceExtensions(&extensionCount);
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.enabledExtensionCount = extensionCount;
        ici.ppEnabledExtensionNames = extensions;
        check(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");

        VkSurfaceKHR surface = VK_NULL_HANDLE;
        check(glfwCreateWindowSurface(instance, window, nullptr, &surface), "glfwCreateWindowSurface");
        uint32_t deviceCount = 0;
        check(vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr), "vkEnumeratePhysicalDevices");
        std::vector<VkPhysicalDevice> devices(deviceCount);
        check(vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data()), "vkEnumeratePhysicalDevices");
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        uint32_t family = 0;
        for (auto d : devices) {
            uint32_t count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(d, &count, nullptr);
            std::vector<VkQueueFamilyProperties> properties(count);
            vkGetPhysicalDeviceQueueFamilyProperties(d, &count, properties.data());
            for (uint32_t f = 0; f < count; ++f) {
                VkBool32 present = VK_FALSE;
                check(vkGetPhysicalDeviceSurfaceSupportKHR(d, f, surface, &present), "surface support");
                if ((properties[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                    physical = d;
                    family = f;
                    break;
                }
            }
            if (physical) break;
        }
        if (!physical) throw std::runtime_error("no present-capable Vulkan device");

        float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = family;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        const char* swapchainExt = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = &swapchainExt;
        VkDevice device = VK_NULL_HANDLE;
        check(vkCreateDevice(physical, &dci, nullptr, &device), "vkCreateDevice");
        VkQueue queue = VK_NULL_HANDLE;
        vkGetDeviceQueue(device, family, 0, &queue);

        VkSurfaceCapabilitiesKHR caps{};
        check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps), "surface capabilities");
        uint32_t formatCount = 0;
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, nullptr), "surface formats");
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, formats.data()), "surface formats");
        if (formats.empty()) throw std::runtime_error("surface has no formats");
        VkExtent2D extent = caps.currentExtent.width == UINT32_MAX ? VkExtent2D{640, 480} : caps.currentExtent;
        VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        sci.surface = surface;
        sci.minImageCount = std::max(2u, caps.minImageCount);
        if (caps.maxImageCount) sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
        sci.imageFormat = formats[0].format;
        sci.imageColorSpace = formats[0].colorSpace;
        sci.imageExtent = extent;
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sci.preTransform = caps.currentTransform;
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        sci.clipped = VK_TRUE;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        check(vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain), "vkCreateSwapchainKHR");
        std::ifstream layerLog(logPath);
        const std::string logText((std::istreambuf_iterator<char>(layerLog)), std::istreambuf_iterator<char>());
        if (logText.find("generation on") == std::string::npos)
            throw std::runtime_error("frame-generation layer did not virtualise the swapchain");
        uint32_t imageCount = 0;
        check(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr), "vkGetSwapchainImagesKHR");
        std::vector<VkImage> images(imageCount);
        check(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, images.data()), "vkGetSwapchainImagesKHR");

        std::vector<VkFence> fences(imageCount + 1, VK_NULL_HANDLE);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        for (auto& fence : fences) check(vkCreateFence(device, &fci, nullptr, &fence), "vkCreateFence");
        std::vector<uint32_t> acquired;
        for (uint32_t i = 0; i < imageCount; ++i) {
            uint32_t index = UINT32_MAX;
            VkResult r = vkAcquireNextImageKHR(device, swapchain, 0, VK_NULL_HANDLE, fences[i], &index);
            if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
                throw std::runtime_error("expected immediate acquisition before exhaustion: " + std::to_string(r));
            check(vkWaitForFences(device, 1, &fences[i], VK_TRUE, UINT64_MAX), "acquire fence");
            if (std::find(acquired.begin(), acquired.end(), index) != acquired.end())
                throw std::runtime_error("acquire returned an image already held by the application");
            acquired.push_back(index);
        }
        uint32_t unused = UINT32_MAX;
        VkResult zero = vkAcquireNextImageKHR(device, swapchain, 0, VK_NULL_HANDLE, fences.back(), &unused);
        const auto start = std::chrono::steady_clock::now();
        VkResult finite = vkAcquireNextImageKHR(device, swapchain, 2'000'000, VK_NULL_HANDLE, fences.back(), &unused);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (zero != VK_NOT_READY || finite != VK_TIMEOUT || elapsed > std::chrono::seconds(2))
            throw std::runtime_error("wrong acquire timeout results: zero=" + std::to_string(zero) +
                                     " finite=" + std::to_string(finite));

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = family;
        VkCommandPool pool = VK_NULL_HANDLE;
        check(vkCreateCommandPool(device, &pci, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        check(vkAllocateCommandBuffers(device, &cai, &cmd), "vkAllocateCommandBuffers");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
        VkClearColorValue colour{{0.0f, 0.0f, 0.0f, 1.0f}};
        for (uint32_t index : acquired) {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = images[index];
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
            vkCmdClearColorImage(cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour,
                                 1, &barrier.subresourceRange);
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = 0;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }
        check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
        check(vkQueueWaitIdle(queue), "vkQueueWaitIdle");
        uint32_t reacquired = UINT32_MAX;
        for (size_t i = 0; i < acquired.size(); ++i) {
            uint32_t index = acquired[i];
            VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            present.swapchainCount = 1;
            present.pSwapchains = &swapchain;
            present.pImageIndices = &index;
            check(vkQueuePresentKHR(queue, &present), "vkQueuePresentKHR");
            if (i == 1) {
                VkResult r = vkAcquireNextImageKHR(device, swapchain, 2'000'000'000,
                                                   VK_NULL_HANDLE, fences.back(), &reacquired);
                if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
                    throw std::runtime_error("acquire did not wake after presentation: " + std::to_string(r));
                check(vkWaitForFences(device, 1, &fences.back(), VK_TRUE, UINT64_MAX), "reacquire fence");
                if (reacquired != acquired[0])
                    throw std::runtime_error("reacquired image was not the first released image");
            }
        }
        VkPresentInfoKHR finalPresent{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        finalPresent.swapchainCount = 1;
        finalPresent.pSwapchains = &swapchain;
        finalPresent.pImageIndices = &reacquired;
        check(vkQueuePresentKHR(queue, &finalPresent), "reacquired image present");
        check(vkDeviceWaitIdle(device), "vkDeviceWaitIdle");
        for (auto fence : fences) vkDestroyFence(device, fence, nullptr);
        vkDestroyCommandPool(device, pool, nullptr);
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyInstance(instance, nullptr);
        glfwDestroyWindow(window);
        glfwTerminate();
        std::remove(logPath.c_str());
        std::puts("zero/finite acquire timeouts and reacquisition: passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "acquire test: %s\n", e.what());
        glfwDestroyWindow(window);
        glfwTerminate();
        if (!logPath.empty()) std::remove(logPath.c_str());
        return 1;
    }
}
