// Two swapchains of one device presented from two threads, on two queues,
// while the config file is rewritten: each present reloads the live settings
// (DeviceData::pollConfig) while the other thread's present reads them. Run
// with the layer enabled; under ThreadSanitizer (the CI "tsan" job) any
// unsynchronised access to the settings is reported. Skips (77) without a
// display or without a queue family offering two queues.
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

static void check(VkResult result, const char* call) {
    if (result < 0) throw std::runtime_error(std::string(call) + ": " + std::to_string(result));
}

namespace {

struct Target {
    GLFWwindow* window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
};

// Acquire, clear, present until `stop` (one thread per target).
void run(VkDevice device, uint32_t family, Target& t, const std::atomic<bool>& stop, std::atomic<bool>& failed) {
    try {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = family;
        check(vkCreateCommandPool(device, &pci, nullptr, &t.pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = t.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        check(vkAllocateCommandBuffers(device, &cai, &cmd), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence acquired = VK_NULL_HANDLE, done = VK_NULL_HANDLE;
        check(vkCreateFence(device, &fci, nullptr, &acquired), "vkCreateFence");
        check(vkCreateFence(device, &fci, nullptr, &done), "vkCreateFence");
        for (int f = 0; !stop && !failed; ++f) {
            uint32_t index = 0;
            check(vkAcquireNextImageKHR(device, t.swapchain, UINT64_MAX, VK_NULL_HANDLE, acquired, &index), "acquire");
            check(vkWaitForFences(device, 1, &acquired, VK_TRUE, UINT64_MAX), "acquire fence");
            check(vkResetFences(device, 1, &acquired), "vkResetFences");
            check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = t.images[index];
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                 0, nullptr, 1, &b);
            const float v = float(f % 60) / 60.0f;
            VkClearColorValue colour{{v, 1.0f - v, 0.5f, 1.0f}};
            vkCmdClearColorImage(cmd, t.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1,
                                 &b.subresourceRange);
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = 0;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &b);
            check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            check(vkQueueSubmit(t.queue, 1, &submit, done), "vkQueueSubmit");
            VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            present.swapchainCount = 1;
            present.pSwapchains = &t.swapchain;
            present.pImageIndices = &index;
            check(vkQueuePresentKHR(t.queue, &present), "vkQueuePresentKHR");
            check(vkWaitForFences(device, 1, &done, VK_TRUE, UINT64_MAX), "submit fence");
            check(vkResetFences(device, 1, &done), "vkResetFences");
        }
        vkQueueWaitIdle(t.queue);
        vkDestroyFence(device, acquired, nullptr);
        vkDestroyFence(device, done, nullptr);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "presenting thread: %s\n", e.what());
        failed = true;
    }
}

void writeConfig(const std::string& path, int k) {
    std::ofstream out(path);
    // Live settings the other thread's present reads while this reloads them.
    out << "enabled = 1\nmultiplier = 2\nlog = 2\n"
        << "zero_bias = " << (k % 2 ? "0.003" : "0.002") << "\n"
        << "smoothness = " << (k % 3 ? "0.004" : "0.005") << "\n"
        << "paused = " << (k % 4 == 3 ? 1 : 0) << "\n"
        << "frame_timing = " << (k % 2) << "\n";
}

} // namespace

int main() {
    if (!glfwInit()) {
        const char* required = std::getenv("BDEX_REQUIRE_DISPLAY");
        if (required && std::string(required) == "1") {
            std::fprintf(stderr, "glfwInit failed with BDEX_REQUIRE_DISPLAY set\n");
            return 1;
        }
        std::puts("no usable display; skipping multi-thread test");
        return 77;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    std::string logPath, configPath;
    int status = 0;
    try {
        char logTemplate[] = "/tmp/bdex-mt-log-XXXXXX";
        int fd = mkstemp(logTemplate);
        if (fd < 0) throw std::runtime_error("could not create layer log");
        close(fd);
        logPath = logTemplate;
        char cfgTemplate[] = "/tmp/bdex-mt-conf-XXXXXX";
        fd = mkstemp(cfgTemplate);
        if (fd < 0) throw std::runtime_error("could not create config file");
        close(fd);
        configPath = cfgTemplate;
        writeConfig(configPath, 0);
        setenv("BDEX_FG_LOG_FILE", logPath.c_str(), 1);
        setenv("BDEX_FG_CONFIG", configPath.c_str(), 1);

        uint32_t extensionCount = 0;
        const char** extensions = glfwGetRequiredInstanceExtensions(&extensionCount);
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.enabledExtensionCount = extensionCount;
        ici.ppEnabledExtensionNames = extensions;
        VkInstance instance = VK_NULL_HANDLE;
        check(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");

        Target targets[2];
        for (int i = 0; i < 2; ++i) {
            targets[i].window = glfwCreateWindow(320, 240, i ? "bdex mt 2" : "bdex mt 1", nullptr, nullptr);
            if (!targets[i].window) throw std::runtime_error("glfwCreateWindow");
            check(glfwCreateWindowSurface(instance, targets[i].window, nullptr, &targets[i].surface), "surface");
        }
        uint32_t deviceCount = 0;
        check(vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr), "vkEnumeratePhysicalDevices");
        std::vector<VkPhysicalDevice> devices(deviceCount);
        check(vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data()), "vkEnumeratePhysicalDevices");
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        uint32_t family = 0;
        for (auto d : devices) {
            uint32_t count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(d, &count, nullptr);
            std::vector<VkQueueFamilyProperties> props(count);
            vkGetPhysicalDeviceQueueFamilyProperties(d, &count, props.data());
            for (uint32_t f = 0; f < count && !physical; ++f) {
                VkBool32 p0 = VK_FALSE, p1 = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(d, f, targets[0].surface, &p0);
                vkGetPhysicalDeviceSurfaceSupportKHR(d, f, targets[1].surface, &p1);
                // Two queues for the game, one more for the layer's private queue.
                if ((props[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) && p0 && p1 && props[f].queueCount >= 2) {
                    physical = d;
                    family = f;
                }
            }
            if (physical) break;
        }
        if (!physical) {
            std::puts("no queue family with two present-capable queues; skipping");
            status = 77;
            throw std::runtime_error("");
        }
        float priorities[2] = {1.0f, 1.0f};
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = family;
        qci.queueCount = 2;
        qci.pQueuePriorities = priorities;
        const char* swapchainExt = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = &swapchainExt;
        VkDevice device = VK_NULL_HANDLE;
        check(vkCreateDevice(physical, &dci, nullptr, &device), "vkCreateDevice");

        for (int i = 0; i < 2; ++i) {
            Target& t = targets[i];
            vkGetDeviceQueue(device, family, uint32_t(i), &t.queue);
            VkSurfaceCapabilitiesKHR caps{};
            check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, t.surface, &caps), "caps");
            uint32_t n = 0;
            check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, t.surface, &n, nullptr), "formats");
            std::vector<VkSurfaceFormatKHR> formats(n);
            check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, t.surface, &n, formats.data()), "formats");
            VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
            sci.surface = t.surface;
            sci.minImageCount = caps.maxImageCount ? std::min(3u, caps.maxImageCount) : 3u;
            if (sci.minImageCount < caps.minImageCount) sci.minImageCount = caps.minImageCount;
            sci.imageFormat = formats[0].format;
            sci.imageColorSpace = formats[0].colorSpace;
            sci.imageExtent = caps.currentExtent.width == UINT32_MAX ? VkExtent2D{320, 240} : caps.currentExtent;
            sci.imageArrayLayers = 1;
            sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            sci.preTransform = caps.currentTransform;
            sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
            sci.clipped = VK_TRUE;
            check(vkCreateSwapchainKHR(device, &sci, nullptr, &t.swapchain), "vkCreateSwapchainKHR");
            check(vkGetSwapchainImagesKHR(device, t.swapchain, &n, nullptr), "images");
            t.images.resize(n);
            check(vkGetSwapchainImagesKHR(device, t.swapchain, &n, t.images.data()), "images");
        }

        std::atomic<bool> failed{false}, stop{false};
        std::thread a([&] { run(device, family, targets[0], stop, failed); });
        std::thread b([&] { run(device, family, targets[1], stop, failed); });
        // Rewrite the config while both present; the layer re-reads it at
        // most every 0.5 s, so several reloads during the 3 s run.
        const auto t0 = std::chrono::steady_clock::now();
        for (int k = 1; std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3); ++k) {
            glfwPollEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            writeConfig(configPath, k);
        }
        stop = true;
        a.join();
        b.join();
        for (auto& t : targets) {
            vkDestroySwapchainKHR(device, t.swapchain, nullptr);
            vkDestroyCommandPool(device, t.pool, nullptr);
        }
        vkDestroyDevice(device, nullptr);
        for (auto& t : targets) vkDestroySurfaceKHR(instance, t.surface, nullptr);
        vkDestroyInstance(instance, nullptr);

        std::ifstream log(logPath);
        const std::string text((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
        size_t generating = 0;
        for (size_t p = 0; (p = text.find("generation on", p)) != std::string::npos; ++p) ++generating;
        const bool reloaded = text.find("live settings reloaded") != std::string::npos;
        std::printf("virtualised swapchains: %zu, live reloads seen: %s\n", generating, reloaded ? "yes" : "no");
        if (failed || generating < 2 || !reloaded) status = 1;
    } catch (const std::exception& e) {
        if (status != 77) {
            std::fprintf(stderr, "multi-thread test failed: %s\n", e.what());
            status = 1;
        }
    }
    if (!logPath.empty()) unlink(logPath.c_str());
    if (!configPath.empty()) unlink(configPath.c_str());
    glfwTerminate();
    return status;
}
