#include "vkctx.h"
#include "log.h"

#include "cap_shaders.h"  // generated: upscale_comp_u32 / _size

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <vector>

namespace bdex {

#define VK_CHECK(x)                                                        \
    do {                                                                   \
        VkResult r_ = (x);                                                 \
        if (r_ != VK_SUCCESS) {                                            \
            LOGE("vulkan error %d at %s:%d", static_cast<int>(r_),         \
                 __FILE__, __LINE__);                                      \
            return false;                                                  \
        }                                                                  \
    } while (0)

static bool hasExt(const char* name, const std::vector<VkExtensionProperties>& list) {
    for (auto& e : list)
        if (std::strcmp(name, e.extensionName) == 0) return true;
    return false;
}

// Vulkan 1.1 core covers the rest (external memory capabilities, dedicated
// allocation, get_physical_device_properties2).
static const char* kDevExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                                 VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
                                 VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
                                 VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME};

// ---------------------------------------------------------------- init ----

bool VkCtx::init(HWND overlay, bool preferFifo) {
    preferFifo_ = preferFifo;

    uint32_t loaderVersion = VK_API_VERSION_1_0;
    vkEnumerateInstanceVersion(&loaderVersion);
    if (loaderVersion < VK_API_VERSION_1_1) {
        LOGE("Vulkan 1.1 instance required (loader reports %u.%u)",
             VK_VERSION_MAJOR(loaderVersion), VK_VERSION_MINOR(loaderVersion));
        return false;
    }
    uint32_t nInst = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &nInst, nullptr);
    std::vector<VkExtensionProperties> instExts(nInst);
    vkEnumerateInstanceExtensionProperties(nullptr, &nInst, instExts.data());
    std::vector<const char*> wantInst = {VK_KHR_SURFACE_EXTENSION_NAME,
                                         VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    for (auto e : wantInst)
        if (!hasExt(e, instExts)) {
            LOGE("instance extension %s missing", e);
            return false;
        }
    std::vector<const char*> layers;
    if (std::getenv("BDEX_CAP_VALIDATION")) layers.push_back("VK_LAYER_KHRONOS_validation");

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "bdex_capture";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<uint32_t>(wantInst.size());
    ici.ppEnabledExtensionNames = wantInst.data();
    ici.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    VK_CHECK(vkCreateInstance(&ici, nullptr, &inst_));

    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = GetModuleHandleW(nullptr);
    sci.hwnd = overlay;
    VK_CHECK(vkCreateWin32SurfaceKHR(inst_, &sci, nullptr, &surface_));

    // GPU: needs GRAPHICS+COMPUTE with present, the external-memory/keyed-mutex
    // extension set, and D3D11-texture import for B8G8R8A8_UNORM SAMPLED /
    // optimal — exactly what phase 1 of docs/windows-capture.md validates.
    uint32_t nPhys = 0;
    vkEnumeratePhysicalDevices(inst_, &nPhys, nullptr);
    std::vector<VkPhysicalDevice> physList(nPhys);
    vkEnumeratePhysicalDevices(inst_, &nPhys, physList.data());
    for (VkPhysicalDevice pd : physList) {
        VkPhysicalDeviceProperties gp;
        vkGetPhysicalDeviceProperties(pd, &gp);
        if (gp.apiVersion < VK_API_VERSION_1_1) {
            LOGW("%s: Vulkan 1.0 only", gp.deviceName);
            continue;
        }

        uint32_t nExt = 0;
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &nExt, nullptr);
        std::vector<VkExtensionProperties> exts(nExt);
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &nExt, exts.data());
        bool ok = true;
        for (auto e : kDevExts)
            if (!hasExt(e, exts)) {
                LOGW("%s: missing %s", gp.deviceName, e);
                ok = false;
            }
        if (!ok) continue;

        uint32_t nQ = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nQ, nullptr);
        std::vector<VkQueueFamilyProperties> qps(nQ);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nQ, qps.data());
        uint32_t qf = UINT32_MAX;
        for (uint32_t i = 0; i < nQ; ++i) {
            VkBool32 surf = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface_, &surf);
            if (surf && (qps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                (qps[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                qf = i;
                break;
            }
        }
        if (qf == UINT32_MAX) {
            LOGW("%s: no GRAPHICS+COMPUTE queue that can present", gp.deviceName);
            continue;
        }

        VkPhysicalDeviceExternalImageFormatInfo extIn{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        extIn.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        VkPhysicalDeviceImageFormatInfo2 fmtIn{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &extIn};
        fmtIn.format = VK_FORMAT_B8G8R8A8_UNORM;
        fmtIn.type = VK_IMAGE_TYPE_2D;
        fmtIn.tiling = VK_IMAGE_TILING_OPTIMAL;
        fmtIn.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        VkExternalImageFormatProperties extOut{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 fmtOut{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &extOut};
        if (vkGetPhysicalDeviceImageFormatProperties2(pd, &fmtIn, &fmtOut) != VK_SUCCESS ||
            !(extOut.externalMemoryProperties.externalMemoryFeatures &
              VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
            LOGW("%s: cannot import D3D11 B8G8R8A8 textures", gp.deviceName);
            continue;
        }

        phys_ = pd;
        qf_ = qf;
        LOGI("GPU: %s (queue family %u, D3D11 import%s)", gp.deviceName, qf,
             (extOut.externalMemoryProperties.externalMemoryFeatures &
              VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT)
                 ? " dedicated-only"
                 : "");
        break;
    }
    if (phys_ == VK_NULL_HANDLE) {
        LOGE("no GPU with D3D11 import + keyed mutex + GRAPHICS/COMPUTE queue");
        return false;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo dq{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    dq.queueFamilyIndex = qf_;
    dq.queueCount = 1;
    dq.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &dq;
    dci.enabledExtensionCount = static_cast<uint32_t>(std::size(kDevExts));
    dci.ppEnabledExtensionNames = kDevExts;
    VK_CHECK(vkCreateDevice(phys_, &dci, nullptr, &dev_));
    vkGetDeviceQueue(dev_, qf_, 0, &queue_);

    getMemProps_ = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
        vkGetDeviceProcAddr(dev_, "vkGetMemoryWin32HandlePropertiesKHR"));
    if (!getMemProps_) {
        LOGE("vkGetMemoryWin32HandlePropertiesKHR missing (old driver?)");
        return false;
    }

    // Sampler the shader assumes: clamp-to-edge + linear, so the kernel's
    // texel-centre fetches return exact texels (see upscale.comp).
    VkSamplerCreateInfo sm{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sm.magFilter = VK_FILTER_LINEAR;
    sm.minFilter = VK_FILTER_LINEAR;
    sm.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sm.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sm.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sm.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(dev_, &sm, nullptr, &sampler_));

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 2;
    dl.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(dev_, &dl, nullptr, &dsl_));

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(UpscalePC)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &dsl_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    VK_CHECK(vkCreatePipelineLayout(dev_, &pl, nullptr, &playout_));

    VkShaderModuleCreateInfo sh{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sh.codeSize = upscale_comp_u32_size;
    sh.pCode = upscale_comp_u32;
    VkShaderModule mod;
    VK_CHECK(vkCreateShaderModule(dev_, &sh, nullptr, &mod));
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = mod;
    stage.pName = "main";
    VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cp.stage = stage;
    cp.layout = playout_;
    VkResult cpr = vkCreateComputePipelines(dev_, VK_NULL_HANDLE, 1, &cp, nullptr, &pipe_);
    vkDestroyShaderModule(dev_, mod, nullptr);
    VK_CHECK(cpr);

    VkDescriptorPoolSize pss[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
                                   {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 1;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = pss;
    VK_CHECK(vkCreateDescriptorPool(dev_, &dp, nullptr, &dpool_));
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = dpool_;
    da.descriptorSetCount = 1;
    da.pSetLayouts = &dsl_;
    VK_CHECK(vkAllocateDescriptorSets(dev_, &da, &dset_));

    VkCommandPoolCreateInfo cp2{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp2.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp2.queueFamilyIndex = qf_;
    VK_CHECK(vkCreateCommandPool(dev_, &cp2, nullptr, &cpool_));
    VkCommandBufferAllocateInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cb.commandPool = cpool_;
    cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb.commandBufferCount = kFrames;
    VK_CHECK(vkAllocateCommandBuffers(dev_, &cb, cbs_));

    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fen{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fen.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t i = 0; i < kFrames; ++i) {
        VK_CHECK(vkCreateSemaphore(dev_, &sem, nullptr, &semImg_[i]));
        VK_CHECK(vkCreateFence(dev_, &fen, nullptr, &fences_[i]));
    }
    LOGI("vulkan ready");
    return true;
}

// -------------------------------------------------------------- sizing ----

const char* VkCtx::presentModeName() const {
    switch (presentMode_) {
    case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo-relaxed";
    default: return "fifo";
    }
}

bool VkCtx::createSwapchain(uint32_t w, uint32_t h) {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps));
    // On Win32 currentExtent follows the window; a 0 extent means minimised.
    if (caps.currentExtent.width != UINT32_MAX) {
        w = caps.currentExtent.width;
        h = caps.currentExtent.height;
    }
    if (w == 0 || h == 0) return false;
    width_ = std::clamp(w, caps.minImageExtent.width, caps.maxImageExtent.width);
    height_ = std::clamp(h, caps.minImageExtent.height, caps.maxImageExtent.height);

    uint32_t nModes = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &nModes, nullptr);
    std::vector<VkPresentModeKHR> modes(nModes);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &nModes, modes.data());
    auto have = [&](VkPresentModeKHR m) {
        return std::find(modes.begin(), modes.end(), m) != modes.end();
    };
    presentMode_ = preferFifo_ ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_MAILBOX_KHR;
    if (!have(presentMode_)) presentMode_ = VK_PRESENT_MODE_FIFO_KHR;

    uint32_t nFmt = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &nFmt, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(nFmt);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &nFmt, fmts.data());
    bool found = false;
    for (auto& f : fmts)
        if (f.format == scFormat_ && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            found = true;
    if (!found) {
        LOGE("B8G8R8A8_UNORM/SRGB surface format not supported");
        return false;
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

    VkSwapchainCreateInfoKHR sw{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sw.surface = surface_;
    sw.minImageCount = imageCount;
    sw.imageFormat = scFormat_;
    sw.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sw.imageExtent = {width_, height_};
    sw.imageArrayLayers = 1;
    sw.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sw.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sw.preTransform = caps.currentTransform;
    sw.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
        sw.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    sw.presentMode = presentMode_;
    sw.clipped = VK_TRUE;
    VK_CHECK(vkCreateSwapchainKHR(dev_, &sw, nullptr, &swapchain_));
    uint32_t nImg = 0;
    vkGetSwapchainImagesKHR(dev_, swapchain_, &nImg, nullptr);
    scImages_.resize(nImg);
    vkGetSwapchainImagesKHR(dev_, swapchain_, &nImg, scImages_.data());
    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semDone_.assign(nImg, VK_NULL_HANDLE);
    for (auto& s : semDone_) VK_CHECK(vkCreateSemaphore(dev_, &sem, nullptr, &s));

    // Packed stage image, same role as framegen.cpp's outReal_: the shader
    // writes u32-encoded pixels, a plain copy lands them in the swapchain
    // image (same 4-byte texel size, no format views needed).
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType = VK_IMAGE_TYPE_2D;
    ic.format = VK_FORMAT_R32_UINT;
    ic.extent = {width_, height_, 1};
    ic.mipLevels = 1;
    ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(dev_, &ic, nullptr, &stage_));
    VkMemoryRequirements reqs;
    vkGetImageMemoryRequirements(dev_, stage_, &reqs);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((reqs.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = i;
            break;
        }
    if (type == UINT32_MAX) {
        LOGE("no DEVICE_LOCAL memory type for the stage image");
        return false;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = reqs.size;
    ai.memoryTypeIndex = type;
    VK_CHECK(vkAllocateMemory(dev_, &ai, nullptr, &stageMem_));
    VK_CHECK(vkBindImageMemory(dev_, stage_, stageMem_, 0));
    VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    iv.image = stage_;
    iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
    iv.format = VK_FORMAT_R32_UINT;
    iv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(dev_, &iv, nullptr, &stageView_));

    LOGI("swapchain %ux%u, %u images, %s", width_, height_, nImg, presentModeName());
    return true;
}

void VkCtx::destroySwapchain() {
    vkDeviceWaitIdle(dev_);
    for (auto s : semDone_) vkDestroySemaphore(dev_, s, nullptr);
    semDone_.clear();
    if (swapchain_) vkDestroySwapchainKHR(dev_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    scImages_.clear();
    if (stageView_) vkDestroyImageView(dev_, stageView_, nullptr);
    if (stage_) vkDestroyImage(dev_, stage_, nullptr);
    if (stageMem_) vkFreeMemory(dev_, stageMem_, nullptr);
    stageView_ = VK_NULL_HANDLE;
    stage_ = VK_NULL_HANDLE;
    stageMem_ = VK_NULL_HANDLE;
    width_ = height_ = 0;
}

bool VkCtx::setSize(uint32_t w, uint32_t h) {
    if (!dev_) return false;
    destroySwapchain();
    if (!createSwapchain(w, h)) {
        destroySwapchain();  // partial state from a failed create
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- draw ----

void VkCtx::destroyImported(SlotVk& sl) {
    vkDeviceWaitIdle(dev_);
    if (sl.view) vkDestroyImageView(dev_, sl.view, nullptr);
    if (sl.image) vkDestroyImage(dev_, sl.image, nullptr);
    if (sl.mem) vkFreeMemory(dev_, sl.mem, nullptr);  // drops our reference to
                                                      // the D3D11 payload
    sl = SlotVk{};
}

bool VkCtx::importSlot(SlotVk& sl, const Capture::Frame& f) {
    VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext};
    ic.imageType = VK_IMAGE_TYPE_2D;
    ic.format = VK_FORMAT_B8G8R8A8_UNORM;
    ic.extent = {f.width, f.height, 1};
    ic.mipLevels = 1;
    ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(dev_, &ic, nullptr, &sl.image));

    VkMemoryRequirements reqs;
    vkGetImageMemoryRequirements(dev_, sl.image, &reqs);
    VkMemoryWin32HandlePropertiesKHR props{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
    VK_CHECK(getMemProps_(dev_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, f.handle,
                          &props));
    uint32_t bits = props.memoryTypeBits & reqs.memoryTypeBits;
    if (bits == 0) bits = props.memoryTypeBits;  // trust the handle over the image
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if (!(bits & (1u << i))) continue;
        if (type == UINT32_MAX) type = i;
        if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            type = i;
            break;
        }
    }
    if (type == UINT32_MAX) {
        LOGE("no memory type for the imported texture");
        return false;
    }
    // D3D11 textures import as dedicated allocations (the handle *is* the
    // texture); allocationSize is ignored for this handle type. The NT handle
    // stays owned by Capture — the import only references the payload.
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = sl.image;
    VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
                                         &ded};
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    imp.handle = f.handle;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp};
    ai.allocationSize = reqs.size;
    ai.memoryTypeIndex = type;
    VK_CHECK(vkAllocateMemory(dev_, &ai, nullptr, &sl.mem));
    VK_CHECK(vkBindImageMemory(dev_, sl.image, sl.mem, 0));

    VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    iv.image = sl.image;
    iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
    iv.format = VK_FORMAT_B8G8R8A8_UNORM;
    iv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(dev_, &iv, nullptr, &sl.view));
    sl.generation = f.generation;
    sl.firstUse = true;
    LOGI("imported ring slot %u (%ux%u, generation %u)", f.slot, f.width, f.height,
         f.generation);
    return true;
}

int VkCtx::draw(const Capture::Frame& f, const DrawParams& params, bool& consumed) {
    consumed = false;
    if (!swapchain_ || f.slot >= Capture::kRingSize) return 1;
    SlotVk& sl = slots_[f.slot];
    if (sl.generation != f.generation) {
        // Ring recreated: free the import of the previous ring (the keyed
        // mutex has long been released by then), let the capture side drop
        // that ring's textures, then import the new handle.
        uint32_t oldGen = sl.generation;
        if (sl.image) destroyImported(sl);
        if (oldGen != UINT32_MAX && cap_) {
            bool others = false;
            for (auto& o : slots_) others = others || (o.generation == oldGen);
            if (!others) cap_->releaseGraveyard(oldGen);
        }
        if (!importSlot(sl, f)) return -1;
    }

    // Wait for this frame's previous submit before touching its command
    // buffer; the fence is only reset once we know we will submit.
    const uint32_t i = static_cast<uint32_t>(frameNo_ % kFrames);
    vkWaitForFences(dev_, 1, &fences_[i], VK_TRUE, UINT64_MAX);

    uint32_t img = 0;
    VkResult ar = vkAcquireNextImageKHR(dev_, swapchain_, UINT64_MAX, semImg_[i],
                                        VK_NULL_HANDLE, &img);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) return 1;
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
        LOGE("vkAcquireNextImageKHR failed (%d)", static_cast<int>(ar));
        return -1;
    }
    vkResetFences(dev_, 1, &fences_[i]);

    VkCommandBuffer cb = cbs_[i];
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);

    // Imported texture: acquire ownership from the external (D3D11) side and
    // make it visible to the compute read. The D3D11 copy that produced it is
    // ordered by the keyed mutex on the submit, the barrier handles caches.
    VkImageMemoryBarrier bImp{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    bImp.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    bImp.dstQueueFamilyIndex = qf_;
    bImp.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bImp.image = sl.image;
    bImp.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    bImp.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    bImp.srcAccessMask = 0;
    bImp.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &bImp);
    sl.firstUse = false;

    // Stage image: ready for the shader write. Swapchain image: ready as a
    // copy destination (contents discarded).
    VkImageMemoryBarrier bPre[2]{};
    bPre[0] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    bPre[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bPre[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bPre[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bPre[0].image = stage_;
    bPre[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bPre[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    bPre[0].srcAccessMask = 0;
    bPre[0].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bPre[1] = bPre[0];
    bPre[1].image = scImages_[img];
    bPre[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bPre[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, bPre);

    // upscale.comp: captured texture -> stage (packed BGRA8 in u32)
    VkDescriptorImageInfo srcInfo{sampler_, sl.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo dstInfo{VK_NULL_HANDLE, stageView_, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[2]{};
    wr[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr[0].dstSet = dset_;
    wr[0].dstBinding = 0;
    wr[0].descriptorCount = 1;
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr[0].pImageInfo = &srcInfo;
    wr[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr[1].dstSet = dset_;
    wr[1].dstBinding = 1;
    wr[1].descriptorCount = 1;
    wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    wr[1].pImageInfo = &dstInfo;
    // Safe to rewrite: the fence wait above guarantees the previous submit
    // that used this set has completed (kFrames == 2 keeps it simple).
    vkUpdateDescriptorSets(dev_, 2, wr, 0, nullptr);

    UpscalePC pc{};
    pc.outSize[0] = static_cast<int32_t>(width_);
    pc.outSize[1] = static_cast<int32_t>(height_);
    pc.srcSize[0] = static_cast<int32_t>(f.width);
    pc.srcSize[1] = static_cast<int32_t>(f.height);
    pc.encoding = 1;  // ENC_BGRA8 (see layer/shaders/upscale.comp)
    pc.filterMode = params.filterMode;
    pc.hudMode = params.hudMode;
    pc.hudGame = params.hudGame;
    pc.hudOut = params.hudOut;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_);
    vkCmdPushConstants(cb, playout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, playout_, 0, 1, &dset_, 0,
                            nullptr);
    vkCmdDispatch(cb, (width_ + 15) / 16, (height_ + 15) / 16, 1);

    // Stage -> swapchain copy.
    VkImageMemoryBarrier bStage{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    bStage.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bStage.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bStage.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bStage.image = stage_;
    bStage.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    bStage.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bStage.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bStage.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bStage);

    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {width_, height_, 1};  // stage and swapchain share the extent
    vkCmdCopyImage(cb, stage_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scImages_[img],
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Swapchain image -> present; imported texture -> back to D3D11.
    VkImageMemoryBarrier bPost[2]{};
    bPost[0] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    bPost[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bPost[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bPost[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bPost[0].image = scImages_[img];
    bPost[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bPost[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    bPost[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bPost[0].dstAccessMask = 0;
    bPost[1] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    bPost[1].srcQueueFamilyIndex = qf_;
    bPost[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    bPost[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bPost[1].image = sl.image;
    bPost[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    bPost[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    bPost[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bPost[1].dstAccessMask = 0;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 2,
                         bPost);

    vkEndCommandBuffer(cb);

    // Keyed mutex on the submit: wait for the D3D11 side's ReleaseSync(1)
    // before the command buffer runs, hand the texture back with key 0 after.
    const uint64_t acquireKey = 1, releaseKey = 0;
    const uint32_t acquireTimeoutMs = 1000;  // the capture thread released 1
                                             // before queuing; longer = a bug
    VkWin32KeyedMutexAcquireReleaseInfoKHR km{
        VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR};
    km.acquireCount = 1;
    km.pAcquireSyncs = &sl.mem;
    km.pAcquireKeys = &acquireKey;
    km.pAcquireTimeouts = &acquireTimeoutMs;
    km.releaseCount = 1;
    km.pReleaseSyncs = &sl.mem;
    km.pReleaseKeys = &releaseKey;

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO, &km};
    sub.waitSemaphoreCount = 1;
    sub.pWaitSemaphores = &semImg_[i];
    sub.pWaitDstStageMask = &waitStage;
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &cb;
    sub.signalSemaphoreCount = 1;
    sub.pSignalSemaphores = &semDone_[img];
    VkResult sr = vkQueueSubmit(queue_, 1, &sub, fences_[i]);
    if (sr != VK_SUCCESS) {
        LOGE("vkQueueSubmit failed (%d)", static_cast<int>(sr));
        return -1;
    }
    consumed = true;  // the release (key 0) is queued behind the compute
    ++frameNo_;

    VkSwapchainKHR sw = swapchain_;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &semDone_[img];
    pi.swapchainCount = 1;
    pi.pSwapchains = &sw;
    pi.pImageIndices = &img;
    VkResult pr = vkQueuePresentKHR(queue_, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) return 1;
    if (pr != VK_SUCCESS) {
        LOGE("vkQueuePresentKHR failed (%d)", static_cast<int>(pr));
        return -1;
    }
    return 0;
}

// ------------------------------------------------------------- shutdown ---

void VkCtx::shutdown() {
    if (!dev_) {
        if (surface_) vkDestroySurfaceKHR(inst_, surface_, nullptr);
        if (inst_) vkDestroyInstance(inst_, nullptr);
        surface_ = VK_NULL_HANDLE;
        inst_ = VK_NULL_HANDLE;
        return;
    }
    vkDeviceWaitIdle(dev_);
    for (auto& sl : slots_) {
        if (sl.view) vkDestroyImageView(dev_, sl.view, nullptr);
        if (sl.image) vkDestroyImage(dev_, sl.image, nullptr);
        if (sl.mem) vkFreeMemory(dev_, sl.mem, nullptr);
        sl = SlotVk{};
    }
    destroySwapchain();
    for (uint32_t i = 0; i < kFrames; ++i) {
        if (semImg_[i]) vkDestroySemaphore(dev_, semImg_[i], nullptr);
        if (fences_[i]) vkDestroyFence(dev_, fences_[i], nullptr);
        semImg_[i] = VK_NULL_HANDLE;
        fences_[i] = VK_NULL_HANDLE;
    }
    if (cpool_) vkDestroyCommandPool(dev_, cpool_, nullptr);
    if (dpool_) vkDestroyDescriptorPool(dev_, dpool_, nullptr);
    if (pipe_) vkDestroyPipeline(dev_, pipe_, nullptr);
    if (playout_) vkDestroyPipelineLayout(dev_, playout_, nullptr);
    if (dsl_) vkDestroyDescriptorSetLayout(dev_, dsl_, nullptr);
    if (sampler_) vkDestroySampler(dev_, sampler_, nullptr);
    cpool_ = VK_NULL_HANDLE;
    dpool_ = VK_NULL_HANDLE;
    pipe_ = VK_NULL_HANDLE;
    playout_ = VK_NULL_HANDLE;
    dsl_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    vkDestroyDevice(dev_, nullptr);
    dev_ = VK_NULL_HANDLE;
    if (surface_) vkDestroySurfaceKHR(inst_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    vkDestroyInstance(inst_, nullptr);
    inst_ = VK_NULL_HANDLE;
    phys_ = VK_NULL_HANDLE;
    LOGI("vulkan shutdown");
}

} // namespace bdex
