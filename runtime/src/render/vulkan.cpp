#include "vulkan.h"

#include <cstdio>

#include "window.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vulkan/vulkan_win32.h>

#include <algorithm>
#include <cstring>

namespace render
{
namespace
{

bool check(VkResult result, std::string *error, const char *what)
{
    if (result == VK_SUCCESS)
        return true;
    if (error)
        *error = std::string(what) + " failed with VkResult " + std::to_string(static_cast<int>(result));
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL reportValidation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                VkDebugUtilsMessageTypeFlagsEXT,
                                                const VkDebugUtilsMessengerCallbackDataEXT *data, void *)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && data && data->pMessage)
        std::fprintf(stderr, "vulkan: %s\n", data->pMessage);
    return VK_FALSE;
}

bool validationAvailable()
{
    std::uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const VkLayerProperties &layer : layers)
        if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0)
            return true;
    return false;
}

} // namespace

bool Device::create(Window &window, std::string *error, bool validation)
{
    m_window = &window;

    // Off unless asked for: the layers cost real time, and a machine without the
    // SDK installed does not have them at all. With them, a mistake in this code
    // says so instead of being drawn wrong or working by luck on one driver.
    const bool wantValidation = validation && validationAvailable();
    if (validation && !wantValidation)
        std::fprintf(stderr, "vulkan: validation asked for but the layer is not installed\n");

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "Genome runtime";
    application.apiVersion = VK_API_VERSION_1_3;

    const char *instanceExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
                                        VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};

    // Ordinary validation checks what a call says; this checks whether one
    // command's writes are really visible to another's reads. Everything the
    // uploads do rests on that, so it is asked for here rather than left to an
    // environment variable which turned out to do nothing.
    const VkValidationFeatureEnableEXT wantedChecks[] = {
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
    VkValidationFeaturesEXT features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    features.enabledValidationFeatureCount = 1;
    features.pEnabledValidationFeatures = wantedChecks;

    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pNext = wantValidation ? &features : nullptr;
    instanceInfo.pApplicationInfo = &application;
    instanceInfo.enabledExtensionCount = wantValidation ? 3 : 2;
    instanceInfo.ppEnabledExtensionNames = instanceExtensions;
    instanceInfo.enabledLayerCount = wantValidation ? 1 : 0;
    instanceInfo.ppEnabledLayerNames = layers;
    if (!check(vkCreateInstance(&instanceInfo, nullptr, &m_instance), error, "vkCreateInstance"))
        return false;

    if (wantValidation)
    {
        auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
        if (createMessenger)
        {
            VkDebugUtilsMessengerCreateInfoEXT messengerInfo{
                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            messengerInfo.pfnUserCallback = reportValidation;
            createMessenger(m_instance, &messengerInfo, nullptr, &m_messenger);
        }
    }

    VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surfaceInfo.hinstance = reinterpret_cast<HINSTANCE>(window.instance());
    surfaceInfo.hwnd = reinterpret_cast<HWND>(window.handle());
    if (!check(vkCreateWin32SurfaceKHR(m_instance, &surfaceInfo, nullptr, &m_surface), error, "vkCreateWin32SurfaceKHR"))
        return false;

    if (!pickPhysicalDevice(error))
        return false;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = m_queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    // Dynamic rendering and synchronisation2 remove a lot of boilerplate that a
    // viewer has no use for.
    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;

    const char *deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.pNext = &features13;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;
    if (!check(vkCreateDevice(m_physicalDevice, &deviceInfo, nullptr, &m_device), error, "vkCreateDevice"))
        return false;

    vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(m_physicalDevice, &properties);
    m_allocationLimit = properties.limits.maxMemoryAllocationCount;

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_queueFamily;
    if (!check(vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool), error, "vkCreateCommandPool"))
        return false;

    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = m_commandPool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = c_FramesInFlight;
    if (!check(vkAllocateCommandBuffers(m_device, &allocate, m_commandBuffers), error, "vkAllocateCommandBuffers"))
        return false;

    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (std::uint32_t index = 0; index < c_FramesInFlight; ++index)
    {
        vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_acquired[index]);
        vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_rendered[index]);
        vkCreateFence(m_device, &fenceInfo, nullptr, &m_inFlight[index]);
    }

    return createSwapchain(error) && createDepth(error);
}

bool Device::pickPhysicalDevice(std::string *error)
{
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

    for (VkPhysicalDevice candidate : devices)
    {
        std::uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());

        for (std::uint32_t family = 0; family < familyCount; ++family)
        {
            VkBool32 supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, family, m_surface, &supported);
            if (!(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !supported)
                continue;

            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            m_physicalDevice = candidate;
            m_queueFamily = family;
            // Keep looking only if this is not a discrete GPU.
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                return true;
        }
    }

    if (m_physicalDevice != VK_NULL_HANDLE)
        return true;
    if (error)
        *error = "no Vulkan device with a graphics queue that can present";
    return false;
}

bool Device::createSwapchain(std::string *error)
{
    VkSurfaceCapabilitiesKHR capabilities{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &capabilities);

    std::uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, formats.data());

    m_surfaceFormat = formats.front();
    for (const VkSurfaceFormatKHR &format : formats)
    {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB || format.format == VK_FORMAT_R8G8B8A8_SRGB)
        {
            m_surfaceFormat = format;
            break;
        }
    }

    m_extent = capabilities.currentExtent;
    if (m_extent.width == 0xFFFFFFFF)
    {
        m_extent.width = static_cast<std::uint32_t>(m_window->width());
        m_extent.height = static_cast<std::uint32_t>(m_window->height());
    }
    if (m_extent.width == 0 || m_extent.height == 0)
        return false; // minimised: nothing to create yet

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = m_surface;
    info.minImageCount = std::max(capabilities.minImageCount, 2u);
    info.imageFormat = m_surfaceFormat.format;
    info.imageColorSpace = m_surfaceFormat.colorSpace;
    info.imageExtent = m_extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    if (!check(vkCreateSwapchainKHR(m_device, &info, nullptr, &m_swapchain), error, "vkCreateSwapchainKHR"))
        return false;

    std::uint32_t imageCount = 0;
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, nullptr);
    m_swapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, m_swapchainImages.data());

    m_swapchainViews.resize(imageCount);
    for (std::uint32_t index = 0; index < imageCount; ++index)
    {
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = m_swapchainImages[index];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = m_surfaceFormat.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(m_device, &viewInfo, nullptr, &m_swapchainViews[index]);
    }
    return true;
}

bool Device::createDepth(std::string *error)
{
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = m_depthFormat;
    imageInfo.extent = {m_extent.width, m_extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (!check(vkCreateImage(m_device, &imageInfo, nullptr, &m_depthImage), error, "vkCreateImage(depth)"))
        return false;

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(m_device, m_depthImage, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!check(vkAllocateMemory(m_device, &allocate, nullptr, &m_depthMemory), error, "vkAllocateMemory(depth)"))
        return false;
    noteAllocation(allocate.allocationSize);
    m_allocationBytes.emplace(m_depthMemory, allocate.allocationSize);
    vkBindImageMemory(m_device, m_depthImage, m_depthMemory, 0);

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = m_depthImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = m_depthFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    return check(vkCreateImageView(m_device, &viewInfo, nullptr, &m_depthView), error, "vkCreateImageView(depth)");
}

void Device::destroySwapchain()
{
    for (VkImageView view : m_swapchainViews)
        vkDestroyImageView(m_device, view, nullptr);
    m_swapchainViews.clear();
    if (m_swapchain)
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
    m_swapchain = VK_NULL_HANDLE;

    if (m_depthView)
        vkDestroyImageView(m_device, m_depthView, nullptr);
    if (m_depthImage)
        vkDestroyImage(m_device, m_depthImage, nullptr);
    if (m_depthMemory)
        freeAllocation(m_depthMemory);
    m_depthView = VK_NULL_HANDLE;
    m_depthImage = VK_NULL_HANDLE;
    m_depthMemory = VK_NULL_HANDLE;
}

bool Device::recreateSwapchain()
{
    vkDeviceWaitIdle(m_device);
    destroySwapchain();
    return createSwapchain(nullptr) && createDepth(nullptr);
}

VkSampler Device::sampler(bool clampToEdge)
{
    VkSampler &cached = clampToEdge ? m_clampSampler : m_repeatSampler;
    if (cached != VK_NULL_HANDLE)
        return cached;

    const VkSamplerAddressMode mode =
        clampToEdge ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;

    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = mode;
    info.addressModeV = mode;
    info.addressModeW = mode;
    info.maxAnisotropy = 1.0f;
    // Whatever the image has: clamping this per texture is what made a sampler
    // per texture look necessary in the first place.
    info.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(m_device, &info, nullptr, &cached);
    return cached;
}

void Device::destroy()
{
    if (!m_device)
        return;
    vkDeviceWaitIdle(m_device);
    if (m_repeatSampler)
        vkDestroySampler(m_device, m_repeatSampler, nullptr);
    if (m_clampSampler)
        vkDestroySampler(m_device, m_clampSampler, nullptr);
    m_repeatSampler = m_clampSampler = VK_NULL_HANDLE;
    destroySwapchain();
    for (std::uint32_t index = 0; index < c_FramesInFlight; ++index)
    {
        vkDestroySemaphore(m_device, m_acquired[index], nullptr);
        vkDestroySemaphore(m_device, m_rendered[index], nullptr);
        vkDestroyFence(m_device, m_inFlight[index], nullptr);
    }
    retireTransfers(true);
    for (VkFence fence : m_spareFences)
        vkDestroyFence(m_device, fence, nullptr);
    m_spareFences.clear();
    vkDestroyCommandPool(m_device, m_commandPool, nullptr);
    vkDestroyDevice(m_device, nullptr);
    if (m_messenger)
    {
        auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyMessenger)
            destroyMessenger(m_instance, m_messenger, nullptr);
    }
    vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    vkDestroyInstance(m_instance, nullptr);
    m_device = VK_NULL_HANDLE;
}

bool Device::beginFrame()
{
    retireTransfers();
    ++m_frameCounter;

    vkWaitForFences(m_device, 1, &m_inFlight[m_frame], VK_TRUE, UINT64_MAX);

    const VkResult acquired =
        vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, m_acquired[m_frame], VK_NULL_HANDLE, &m_imageIndex);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR || acquired == VK_SUBOPTIMAL_KHR)
        return false;

    vkResetFences(m_device, 1, &m_inFlight[m_frame]);
    vkResetCommandBuffer(m_commandBuffers[m_frame], 0);

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m_commandBuffers[m_frame], &begin);
    return true;
}

void Device::endFrame()
{
    vkEndCommandBuffer(m_commandBuffers[m_frame]);

    const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &m_acquired[m_frame];
    submit.pWaitDstStageMask = &wait;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &m_commandBuffers[m_frame];
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &m_rendered[m_frame];
    vkQueueSubmit(m_queue, 1, &submit, m_inFlight[m_frame]);

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &m_rendered[m_frame];
    present.swapchainCount = 1;
    present.pSwapchains = &m_swapchain;
    present.pImageIndices = &m_imageIndex;
    vkQueuePresentKHR(m_queue, &present);

    m_frame = (m_frame + 1) % c_FramesInFlight;
}

std::uint32_t Device::findMemoryType(std::uint32_t mask, VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memory);
    for (std::uint32_t index = 0; index < memory.memoryTypeCount; ++index)
    {
        if ((mask & (1u << index)) && (memory.memoryTypes[index].propertyFlags & properties) == properties)
            return index;
    }
    // Zero is a memory type, not a sentinel. On this device type 0 is host
    // memory that is nonetheless usable for optimally tiled colour images, so
    // returning it on a miss puts a texture meant for the card into system RAM
    // and says nothing about it.
    return c_NoMemoryType;
}

void Device::noteAllocation(VkDeviceSize bytes)
{
    ++m_liveAllocations;
    m_peakAllocations = std::max(m_peakAllocations, m_liveAllocations);
    m_liveBytes += bytes;
    m_peakBytes = std::max(m_peakBytes, m_liveBytes);
}

bool Device::allocate(const VkMemoryRequirements &requirements, bool hostVisible, VkDeviceMemory &memory,
                      std::string *error)
{
    const VkMemoryPropertyFlags properties =
        hostVisible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                    : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.allocationSize = requirements.size;
    info.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, properties);
    if (info.memoryTypeIndex == c_NoMemoryType)
    {
        if (error)
            *error = "no memory type on this device has what was asked for";
        return false;
    }
    if (!check(vkAllocateMemory(m_device, &info, nullptr, &memory), error, "vkAllocateMemory"))
        return false;
    noteAllocation(info.allocationSize);
    m_allocationBytes.emplace(memory, info.allocationSize);
    return true;
}

bool Device::capture(const char *path, std::string *error)
{
    const auto fail = [&](const char *reason) {
        if (error)
            *error = reason;
        return false;
    };

    vkDeviceWaitIdle(m_device);

    const VkDeviceSize size = VkDeviceSize(m_extent.width) * m_extent.height * 4;
    Buffer staging = createBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, error);
    if (!staging.handle)
        return false;

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_swapchainImages[m_imageIndex];
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkCommandBuffer command = beginOneShot();

    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {m_extent.width, m_extent.height, 1};
    vkCmdCopyImageToBuffer(command, m_swapchainImages[m_imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           staging.handle, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);

    endOneShot(command);

    std::FILE *file = std::fopen(path, "wb");
    if (!file)
    {
        destroyBuffer(staging);
        return fail("cannot write the capture");
    }

    std::fprintf(file, "P6\n%u %u\n255\n", m_extent.width, m_extent.height);
    const bool blue_first = m_surfaceFormat.format == VK_FORMAT_B8G8R8A8_UNORM ||
                            m_surfaceFormat.format == VK_FORMAT_B8G8R8A8_SRGB;
    const auto *pixels = static_cast<const std::uint8_t *>(staging.mapped);
    std::vector<std::uint8_t> row(std::size_t(m_extent.width) * 3);
    for (std::uint32_t y = 0; y < m_extent.height; ++y)
    {
        const std::uint8_t *source = pixels + std::size_t(y) * m_extent.width * 4;
        for (std::uint32_t x = 0; x < m_extent.width; ++x)
        {
            row[x * 3 + 0] = source[x * 4 + (blue_first ? 2 : 0)];
            row[x * 3 + 1] = source[x * 4 + 1];
            row[x * 3 + 2] = source[x * 4 + (blue_first ? 0 : 2)];
        }
        std::fwrite(row.data(), 1, row.size(), file);
    }
    std::fclose(file);

    destroyBuffer(staging);
    return true;
}

VkCommandBuffer Device::beginOneShot()
{
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = m_commandPool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_device, &allocate, &command);

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(command, &begin);
    return command;
}

void Device::barrierAfterTransfer(VkCommandBuffer command)
{
    // A barrier outside a render pass orders against everything later in
    // submission order on this queue, which includes later submissions - that
    // is what lets the copy go unwaited.
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                            VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void Device::endOneShotAsync(VkCommandBuffer command, const std::vector<Buffer> &recycle)
{
    barrierAfterTransfer(command);
    vkEndCommandBuffer(command);

    // Keep the number in flight bounded. Waiting here is the same drain that
    // was being paid every time, so it is counted and reported rather than
    // hidden - if it ever fires often, the bound is wrong.
    while (m_retiring.size() >= c_MaxTransfersInFlight)
    {
        ++m_transferStalls;
        retireTransfers(true);
    }

    VkFence fence = VK_NULL_HANDLE;
    if (!m_spareFences.empty())
    {
        fence = m_spareFences.back();
        m_spareFences.pop_back();
        vkResetFences(m_device, 1, &fence);
    }
    else
    {
        VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(m_device, &info, nullptr, &fence);
    }

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    vkQueueSubmit(m_queue, 1, &submit, fence);

    m_retiring.push_back({fence, command, recycle});
}

void Device::retireTransfers(bool waitForAll)
{
    for (std::size_t index = 0; index < m_retiring.size();)
    {
        Retiring &one = m_retiring[index];
        if (waitForAll)
            vkWaitForFences(m_device, 1, &one.fence, VK_TRUE, UINT64_MAX);
        else if (vkGetFenceStatus(m_device, one.fence) != VK_SUCCESS)
        {
            ++index;
            continue;
        }

        for (Buffer &buffer : one.recycle)
            destroyBuffer(buffer);
        vkFreeCommandBuffers(m_device, m_commandPool, 1, &one.command);
        m_spareFences.push_back(one.fence);
        m_retiring.erase(m_retiring.begin() + std::ptrdiff_t(index));
        if (waitForAll)
            index = 0;
    }
}

void Device::endOneShot(VkCommandBuffer command)
{
    vkEndCommandBuffer(command);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    vkQueueSubmit(m_queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_queue);
    vkFreeCommandBuffers(m_device, m_commandPool, 1, &command);
}

Buffer Device::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, std::string *error)
{
    Buffer buffer;
    buffer.size = size;

    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!check(vkCreateBuffer(m_device, &info, nullptr, &buffer.handle), error, "vkCreateBuffer"))
        return buffer;

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(m_device, buffer.handle, &requirements);

    const VkMemoryPropertyFlags properties =
        hostVisible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                    : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, properties);
    if (allocate.memoryTypeIndex == c_NoMemoryType)
    {
        if (error)
            *error = "no memory type on this device has what a buffer asked for";
        vkDestroyBuffer(m_device, buffer.handle, nullptr);
        buffer = {};
        return buffer;
    }
    if (!check(vkAllocateMemory(m_device, &allocate, nullptr, &buffer.memory), error, "vkAllocateMemory"))
        return buffer;
    noteAllocation(allocate.allocationSize);
    m_allocationBytes.emplace(buffer.memory, allocate.allocationSize);
    vkBindBufferMemory(m_device, buffer.handle, buffer.memory, 0);

    if (hostVisible)
        vkMapMemory(m_device, buffer.memory, 0, size, 0, &buffer.mapped);
    return buffer;
}

void Device::freeAllocation(VkDeviceMemory &memory)
{
    if (memory == VK_NULL_HANDLE)
        return;
    const auto known = m_allocationBytes.find(memory);
    if (known != m_allocationBytes.end())
    {
        m_liveBytes -= std::min(m_liveBytes, known->second);
        m_allocationBytes.erase(known);
    }
    vkFreeMemory(m_device, memory, nullptr);
    memory = VK_NULL_HANDLE;
    m_liveAllocations -= std::min<std::size_t>(m_liveAllocations, 1);
}

void Device::destroyBuffer(Buffer &buffer)
{

    if (buffer.mapped)
        vkUnmapMemory(m_device, buffer.memory);
    if (buffer.handle)
        vkDestroyBuffer(m_device, buffer.handle, nullptr);
    if (buffer.memory)
        freeAllocation(buffer.memory);
    buffer = {};
}

} // namespace render
