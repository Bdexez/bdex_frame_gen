// Minimal Vulkan + GLFW test application: renders a procedural scene with
// moving objects at a capped frame rate so that the frame generation layer
// can be exercised without a game.
//
//   bdex_demo [--fps N] [--frames N] [--size WxH] [--mode fifo|mailbox|immediate]
//             [--deterministic] [--speed X]
//
// Run it with BDEX_FG=1 (and VK_LAYER_PATH pointing at the build tree if the
// layer is not installed) to see the generated frames.
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "demo_shaders.h"

#define VK_CHECK(x)                                                                             \
    do {                                                                                        \
        VkResult _r = (x);                                                                      \
        if (_r < 0) throw std::runtime_error(std::string(#x) + " failed: " + std::to_string(_r)); \
    } while (0)

namespace {

struct Options {
    double fps = 30.0;
    long frames = -1;
    int width = 1280, height = 720;
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    bool deterministic = false;  // animation time advances by 1/fps per frame instead of wall clock
    bool srgb = false;           // request an sRGB swapchain format
    float speed = 1.0f;          // animation speed multiplier
};

struct PushConstants {
    float resolution[2];
    float time;
    uint32_t frame;
};

class Demo {
public:
    explicit Demo(const Options& o) : opt_(o) {}
    ~Demo() { cleanup(); }
    void run();

private:
    void initWindow();
    void initVulkan();
    void createSwapchain();
    void destroySwapchain();
    void createPipeline();
    void drawFrame();
    void cleanup();

    Options opt_;
    GLFWwindow* window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physDev_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkFramebuffer> framebuffers_;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;

    static constexpr int kInFlight = 2;
    VkCommandBuffer cmds_[kInFlight]{};
    VkSemaphore acquireSem_[kInFlight]{};
    std::vector<VkSemaphore> renderSem_;  // per swapchain image
    VkFence fences_[kInFlight]{};
    int frameIndex_ = 0;
    uint32_t frameCount_ = 0;
    bool resized_ = false;
};

void Demo::initWindow() {
    if (!glfwInit()) throw std::runtime_error("glfwInit failed");
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window_ = glfwCreateWindow(opt_.width, opt_.height, "bdex frame generation demo", nullptr, nullptr);
    if (!window_) throw std::runtime_error("glfwCreateWindow failed");
    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, [](GLFWwindow* w, int, int) {
        static_cast<Demo*>(glfwGetWindowUserPointer(w))->resized_ = true;
    });
}

void Demo::initVulkan() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "bdex_demo";
    app.apiVersion = VK_API_VERSION_1_1;
    uint32_t extCount = 0;
    const char** exts = glfwGetRequiredInstanceExtensions(&extCount);
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = extCount;
    ici.ppEnabledExtensionNames = exts;
    VK_CHECK(vkCreateInstance(&ici, nullptr, &instance_));
    VK_CHECK(glfwCreateWindowSurface(instance_, window_, nullptr, &surface_));

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(instance_, &n, devs.data());
    for (VkPhysicalDevice d : devs) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qf.data());
        for (uint32_t i = 0; i < qn; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &present);
            if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                physDev_ = d;
                queueFamily_ = i;
                break;
            }
        }
        if (physDev_) break;
    }
    if (!physDev_) throw std::runtime_error("no suitable GPU");
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physDev_, &props);
    printf("demo: using %s\n", props.deviceName);

    float prio = 1.f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* devExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = devExts;
    VK_CHECK(vkCreateDevice(physDev_, &dci, nullptr, &device_));
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily_;
    VK_CHECK(vkCreateCommandPool(device_, &pci, nullptr, &pool_));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = kInFlight;
    VK_CHECK(vkAllocateCommandBuffers(device_, &cai, cmds_));
    for (int i = 0; i < kInFlight; ++i) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(device_, &si, nullptr, &acquireSem_[i]));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(device_, &fi, nullptr, &fences_[i]));
    }

    createSwapchain();
    createPipeline();
}

void Demo::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physDev_, surface_, &caps));
    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physDev_, surface_, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fn);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physDev_, surface_, &fn, formats.data());
    VkSurfaceFormatKHR chosen = formats[0];
    const VkFormat wanted = opt_.srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
    for (auto& f : formats)
        if (f.format == wanted && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) chosen = f;
    format_ = chosen.format;

    uint32_t mn = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physDev_, surface_, &mn, nullptr);
    std::vector<VkPresentModeKHR> modes(mn);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physDev_, surface_, &mn, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (std::find(modes.begin(), modes.end(), opt_.mode) != modes.end()) mode = opt_.mode;

    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    if (caps.currentExtent.width != UINT32_MAX) extent_ = caps.currentExtent;
    else extent_ = {std::clamp<uint32_t>(w, caps.minImageExtent.width, caps.maxImageExtent.width),
                    std::clamp<uint32_t>(h, caps.minImageExtent.height, caps.maxImageExtent.height)};

    uint32_t count = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface_;
    sci.minImageCount = count;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent_;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = swapchain_;
    VkSwapchainKHR old = swapchain_;
    VK_CHECK(vkCreateSwapchainKHR(device_, &sci, nullptr, &swapchain_));
    if (old) vkDestroySwapchainKHR(device_, old, nullptr);

    uint32_t in = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &in, nullptr);
    images_.resize(in);
    vkGetSwapchainImagesKHR(device_, swapchain_, &in, images_.data());
    printf("demo: swapchain %ux%u, %u images, present mode %d\n", extent_.width, extent_.height, in, mode);

    if (!renderPass_) {
        VkAttachmentDescription att{};
        att.format = format_;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
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
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = 0;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rci.attachmentCount = 1;
        rci.pAttachments = &att;
        rci.subpassCount = 1;
        rci.pSubpasses = &sub;
        rci.dependencyCount = 1;
        rci.pDependencies = &dep;
        VK_CHECK(vkCreateRenderPass(device_, &rci, nullptr, &renderPass_));
    }

    views_.resize(in);
    framebuffers_.resize(in);
    for (uint32_t i = 0; i < in; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = images_[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format_;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &views_[i]));
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = renderPass_;
        fci.attachmentCount = 1;
        fci.pAttachments = &views_[i];
        fci.width = extent_.width;
        fci.height = extent_.height;
        fci.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device_, &fci, nullptr, &framebuffers_[i]));
    }
    for (VkSemaphore s : renderSem_) vkDestroySemaphore(device_, s, nullptr);
    renderSem_.assign(in, VK_NULL_HANDLE);
    for (auto& s : renderSem_) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(device_, &si, nullptr, &s));
    }
}

void Demo::destroySwapchain() {
    for (VkFramebuffer f : framebuffers_) vkDestroyFramebuffer(device_, f, nullptr);
    for (VkImageView v : views_) vkDestroyImageView(device_, v, nullptr);
    framebuffers_.clear();
    views_.clear();
}

void Demo::createPipeline() {
    auto makeModule = [&](const uint32_t* code, size_t size) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = size;
        ci.pCode = code;
        VkShaderModule m;
        VK_CHECK(vkCreateShaderModule(device_, &ci, nullptr, &m));
        return m;
    };
    VkShaderModule vs = makeModule(fullscreen_vert, fullscreen_vert_size);
    VkShaderModule fs = makeModule(scene_frag, scene_frag_size);

    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &pcr;
    VK_CHECK(vkCreatePipelineLayout(device_, &lci, nullptr, &layout_));

    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &ds;
    pci.layout = layout_;
    pci.renderPass = renderPass_;
    VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline_));
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
}

void Demo::drawFrame() {
    static const auto start = std::chrono::steady_clock::now();
    const int fi = frameIndex_;
    vkWaitForFences(device_, 1, &fences_[fi], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, acquireSem_[fi], VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        vkDeviceWaitIdle(device_);
        destroySwapchain();
        createSwapchain();
        return;
    }
    if (r < 0) throw std::runtime_error("vkAcquireNextImageKHR failed");
    vkResetFences(device_, 1, &fences_[fi]);

    VkCommandBuffer cmd = cmds_[fi];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = renderPass_;
    rbi.framebuffer = framebuffers_[imageIndex];
    rbi.renderArea = {{0, 0}, extent_};
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{0.f, 0.f, float(extent_.width), float(extent_.height), 0.f, 1.f};
    VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    const double animTime = opt_.deterministic
                                ? frameCount_ / opt_.fps
                                : std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    PushConstants pc{{float(extent_.width), float(extent_.height)}, float(animTime * opt_.speed), frameCount_};
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquireSem_[fi];
    si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &renderSem_[imageIndex];
    VK_CHECK(vkQueueSubmit(queue_, 1, &si, fences_[fi]));

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderSem_[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex;
    r = vkQueuePresentKHR(queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || resized_) {
        resized_ = false;
        vkDeviceWaitIdle(device_);
        destroySwapchain();
        createSwapchain();
    } else if (r < 0) {
        throw std::runtime_error("vkQueuePresentKHR failed");
    }
    frameIndex_ = (frameIndex_ + 1) % kInFlight;
    ++frameCount_;
}

void Demo::run() {
    initWindow();
    initVulkan();
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / opt_.fps));
    auto next = clock::now();
    auto statT = clock::now();
    uint32_t statFrames = 0;
    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        if (glfwGetKey(window_, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
        int w = 0, h = 0;
        glfwGetFramebufferSize(window_, &w, &h);
        if (w == 0 || h == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
        drawFrame();
        ++statFrames;
        if (opt_.frames >= 0 && frameCount_ >= static_cast<uint32_t>(opt_.frames)) break;
        const auto now = clock::now();
        if (std::chrono::duration<double>(now - statT).count() >= 2.0) {
            char title[128];
            snprintf(title, sizeof(title), "bdex demo - %.1f fps rendered (target %.0f)",
                     statFrames / std::chrono::duration<double>(now - statT).count(), opt_.fps);
            glfwSetWindowTitle(window_, title);
            printf("%s\n", title);
            statT = now;
            statFrames = 0;
        }
        next += period;
        if (next > clock::now()) std::this_thread::sleep_until(next);
        else next = clock::now();
    }
    vkDeviceWaitIdle(device_);
}

void Demo::cleanup() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        destroySwapchain();
        for (VkSemaphore s : renderSem_) vkDestroySemaphore(device_, s, nullptr);
        for (int i = 0; i < kInFlight; ++i) {
            if (acquireSem_[i]) vkDestroySemaphore(device_, acquireSem_[i], nullptr);
            if (fences_[i]) vkDestroyFence(device_, fences_[i], nullptr);
        }
        if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
        if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
        if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);
        if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
    if (window_) glfwDestroyWindow(window_);
    glfwTerminate();
}

Options parseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
            return argv[++i];
        };
        if (a == "--fps") o.fps = std::max(1.0, atof(value().c_str()));
        else if (a == "--frames") o.frames = atol(value().c_str());
        else if (a == "--deterministic") o.deterministic = true;
        else if (a == "--srgb") o.srgb = true;
        else if (a == "--speed") o.speed = atof(value().c_str());
        else if (a == "--size") {
            std::string v = value();
            if (sscanf(v.c_str(), "%dx%d", &o.width, &o.height) != 2) throw std::runtime_error("bad --size");
        } else if (a == "--mode") {
            std::string v = value();
            if (v == "fifo") o.mode = VK_PRESENT_MODE_FIFO_KHR;
            else if (v == "mailbox") o.mode = VK_PRESENT_MODE_MAILBOX_KHR;
            else if (v == "immediate") o.mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            else throw std::runtime_error("bad --mode");
        } else if (a == "-h" || a == "--help") {
            printf("usage: bdex_demo [--fps N] [--frames N] [--size WxH] [--mode fifo|mailbox|immediate]\n"
                   "                 [--deterministic] [--speed X] [--srgb]\n");
            exit(0);
        } else {
            throw std::runtime_error("unknown option " + a);
        }
    }
    return o;
}

} // namespace

int main(int argc, char** argv) {
    try {
        Demo demo(parseArgs(argc, argv));
        demo.run();
    } catch (const std::exception& e) {
        fprintf(stderr, "demo: %s\n", e.what());
        return 1;
    }
    return 0;
}
