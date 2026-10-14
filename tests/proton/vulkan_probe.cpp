// A minimal Win32 Vulkan game for tools/ci/wine-layer-check.sh: the Windows
// Vulkan loader (vulkan-1.dll, loaded at run time), a window, a swapchain, a
// bar sliding over a cleared background at 30 fps for a few seconds. Run
// under Wine with Khronos' loader and Wine's winevulkan as its driver, it
// loads the Windows build of the layer as a Windows game would.
//   x86_64-w64-mingw32-g++ -O2 -static vulkan_probe.cpp -o vulkan_probe.exe
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcA(h, m, w, l);
}

#define FN(name) static PFN_##name name
FN(vkGetInstanceProcAddr); FN(vkCreateInstance); FN(vkEnumeratePhysicalDevices); FN(vkGetPhysicalDeviceQueueFamilyProperties);
FN(vkCreateWin32SurfaceKHR); FN(vkGetPhysicalDeviceSurfaceSupportKHR); FN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
FN(vkGetPhysicalDeviceSurfaceFormatsKHR); FN(vkCreateDevice); FN(vkGetDeviceProcAddr); FN(vkGetPhysicalDeviceProperties);
FN(vkGetDeviceQueue); FN(vkCreateSwapchainKHR); FN(vkGetSwapchainImagesKHR); FN(vkCreateImageView); FN(vkCreateRenderPass);
FN(vkCreateFramebuffer); FN(vkCreateCommandPool); FN(vkAllocateCommandBuffers); FN(vkCreateSemaphore); FN(vkCreateFence);
FN(vkWaitForFences); FN(vkResetFences); FN(vkAcquireNextImageKHR); FN(vkBeginCommandBuffer); FN(vkCmdBeginRenderPass);
FN(vkCmdClearAttachments); FN(vkCmdEndRenderPass); FN(vkEndCommandBuffer); FN(vkQueueSubmit); FN(vkQueuePresentKHR);
FN(vkDeviceWaitIdle); FN(vkResetCommandBuffer);

#define CHECK(x)                                                     \
    do {                                                             \
        VkResult r_ = (x);                                           \
        if (r_ < 0) {                                                \
            printf("%s failed: %d\n", #x, (int)r_);                  \
            return 1;                                                \
        }                                                            \
    } while (0)

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? atoi(argv[1]) : 8;
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (!lib) { printf("vulkan-1.dll not found\n"); return 1; }
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void*)GetProcAddress(lib, "vkGetInstanceProcAddr");
#define IG(name) name = (PFN_##name)vkGetInstanceProcAddr(inst, #name)
    VkInstance inst = VK_NULL_HANDLE;
    IG(vkCreateInstance);
    const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "vulkan_probe";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = exts;
    CHECK(vkCreateInstance(&ici, nullptr, &inst));
    IG(vkEnumeratePhysicalDevices); IG(vkGetPhysicalDeviceQueueFamilyProperties); IG(vkCreateWin32SurfaceKHR);
    IG(vkGetPhysicalDeviceSurfaceSupportKHR); IG(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); IG(vkGetPhysicalDeviceProperties);
    IG(vkGetPhysicalDeviceSurfaceFormatsKHR); IG(vkCreateDevice); IG(vkGetDeviceProcAddr);

    WNDCLASSA wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "bdex_vkprobe";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("bdex_vkprobe", "bdex vulkan probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 640, 360,
                              nullptr, nullptr, wc.hInstance, nullptr);
    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = wc.hInstance;
    sci.hwnd = hwnd;
    VkSurfaceKHR surface;
    CHECK(vkCreateWin32SurfaceKHR(inst, &sci, nullptr, &surface));

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(inst, &n, pds.data());
    if (n == 0) { printf("no Vulkan device\n"); return 1; }
    VkPhysicalDevice pd = pds[0];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("device: %s\n", props.deviceName);
    uint32_t qn = 0, qf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qs.data());
    for (uint32_t i = 0; i < qn; ++i) {
        VkBool32 ok = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &ok);
        if (ok && (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { qf = i; break; }
    }
    float prio = 1.f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qf;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dexts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dexts;
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &dci, nullptr, &dev));
#define DG(name) name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    DG(vkGetDeviceQueue); DG(vkCreateSwapchainKHR); DG(vkGetSwapchainImagesKHR); DG(vkCreateImageView); DG(vkCreateRenderPass);
    DG(vkCreateFramebuffer); DG(vkCreateCommandPool); DG(vkAllocateCommandBuffers); DG(vkCreateSemaphore); DG(vkCreateFence);
    DG(vkWaitForFences); DG(vkResetFences); DG(vkAcquireNextImageKHR); DG(vkBeginCommandBuffer); DG(vkCmdBeginRenderPass);
    DG(vkCmdClearAttachments); DG(vkCmdEndRenderPass); DG(vkEndCommandBuffer); DG(vkQueueSubmit); DG(vkQueuePresentKHR);
    DG(vkDeviceWaitIdle); DG(vkResetCommandBuffer);
    VkQueue queue;
    vkGetDeviceQueue(dev, qf, 0, &queue);

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);
    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fn);
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &fn, fmts.data());
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : VkExtent2D{640, 360};
    VkSwapchainCreateInfoKHR sw{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sw.surface = surface;
    sw.minImageCount = caps.minImageCount + 1;
    sw.imageFormat = fmts[0].format;
    sw.imageColorSpace = fmts[0].colorSpace;
    sw.imageExtent = ext;
    sw.imageArrayLayers = 1;
    sw.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sw.preTransform = caps.currentTransform;
    sw.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sw.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sw.clipped = VK_TRUE;
    VkSwapchainKHR swapchain;
    CHECK(vkCreateSwapchainKHR(dev, &sw, nullptr, &swapchain));
    uint32_t in = 0;
    vkGetSwapchainImagesKHR(dev, swapchain, &in, nullptr);
    std::vector<VkImage> images(in);
    vkGetSwapchainImagesKHR(dev, swapchain, &in, images.data());

    VkAttachmentDescription att{};
    att.format = sw.imageFormat;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0};
    VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rci.attachmentCount = 1;
    rci.pAttachments = &att;
    rci.subpassCount = 1;
    rci.pSubpasses = &sub;
    rci.dependencyCount = 1;
    rci.pDependencies = &dep;
    VkRenderPass rp;
    CHECK(vkCreateRenderPass(dev, &rci, nullptr, &rp));
    std::vector<VkFramebuffer> fbs(in);
    for (uint32_t i = 0; i < in; ++i) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = sw.imageFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view;
        CHECK(vkCreateImageView(dev, &vi, nullptr, &view));
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = rp;
        fci.attachmentCount = 1;
        fci.pAttachments = &view;
        fci.width = ext.width;
        fci.height = ext.height;
        fci.layers = 1;
        CHECK(vkCreateFramebuffer(dev, &fci, nullptr, &fbs[i]));
    }
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = qf;
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(dev, &pci, nullptr, &pool));
    const int kInFlight = 2;
    VkCommandBuffer cmds[kInFlight];
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = kInFlight;
    CHECK(vkAllocateCommandBuffers(dev, &cai, cmds));
    VkSemaphore acq[kInFlight];
    std::vector<VkSemaphore> done(in);
    VkFence fences[kInFlight];
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < kInFlight; ++i) {
        CHECK(vkCreateSemaphore(dev, &si, nullptr, &acq[i]));
        CHECK(vkCreateFence(dev, &fi, nullptr, &fences[i]));
    }
    for (auto& s : done) CHECK(vkCreateSemaphore(dev, &si, nullptr, &s));

    const int frames = seconds * 30;
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    for (int f = 0; f < frames; ++f) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        const int k = f % kInFlight;
        vkWaitForFences(dev, 1, &fences[k], VK_TRUE, UINT64_MAX);
        uint32_t img = 0;
        VkResult ar = vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, acq[k], VK_NULL_HANDLE, &img);
        if (ar < 0) { printf("acquire failed: %d\n", (int)ar); return 1; }
        vkResetFences(dev, 1, &fences[k]);
        VkCommandBuffer cb = cmds[k];
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cb, &bi);
        VkClearValue bg{};
        bg.color = {{0.1f, 0.12f, 0.15f, 1.f}};
        VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rbi.renderPass = rp;
        rbi.framebuffer = fbs[img];
        rbi.renderArea = {{0, 0}, ext};
        rbi.clearValueCount = 1;
        rbi.pClearValues = &bg;
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
        ca.clearValue.color = {{0.9f, 0.8f, 0.2f, 1.f}};
        const int32_t x = (f * 8) % int32_t(ext.width > 48 ? ext.width - 40 : 1);  // a bar sliding 8 px a frame
        VkClearRect cr{{{x, int32_t(ext.height / 4)}, {40, ext.height / 2}}, 0, 1};
        vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
        vkCmdEndRenderPass(cb);
        vkEndCommandBuffer(cb);
        VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo sub2{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        sub2.waitSemaphoreCount = 1;
        sub2.pWaitSemaphores = &acq[k];
        sub2.pWaitDstStageMask = &wait;
        sub2.commandBufferCount = 1;
        sub2.pCommandBuffers = &cb;
        sub2.signalSemaphoreCount = 1;
        sub2.pSignalSemaphores = &done[img];
        CHECK(vkQueueSubmit(queue, 1, &sub2, fences[k]));
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &done[img];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &img;
        VkResult pr = vkQueuePresentKHR(queue, &pi);
        if (pr < 0) { printf("present failed: %d\n", (int)pr); return 1; }
        const LONGLONG due = start.QuadPart + (LONGLONG)(f + 1) * freq.QuadPart / 30;  // 30 fps
        for (;;) {
            QueryPerformanceCounter(&now);
            if (now.QuadPart >= due) break;
            Sleep(1);
        }
    }
    vkDeviceWaitIdle(dev);
    printf("vulkan probe: %d frames presented\n", frames);
    return 0;
}
