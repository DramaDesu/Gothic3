#include "vulkan.h"

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

} // namespace

bool Device::create(Window &window, std::string *error)
{
    m_window = &window;

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "Genome runtime";
    application.apiVersion = VK_API_VERSION_1_3;

    const char *instanceExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &application;
    instanceInfo.enabledExtensionCount = 2;
    instanceInfo.ppEnabledExtensionNames = instanceExtensions;
    if (!check(vkCreateInstance(&instanceInfo, nullptr, &m_instance), error, "vkCreateInstance"))
        return false;

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
        vkFreeMemory(m_device, m_depthMemory, nullptr);
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

void Device::destroy()
{
    if (!m_device)
        return;
    vkDeviceWaitIdle(m_device);
    destroySwapchain();
    for (std::uint32_t index = 0; index < c_FramesInFlight; ++index)
    {
        vkDestroySemaphore(m_device, m_acquired[index], nullptr);
        vkDestroySemaphore(m_device, m_rendered[index], nullptr);
        vkDestroyFence(m_device, m_inFlight[index], nullptr);
    }
    vkDestroyCommandPool(m_device, m_commandPool, nullptr);
    vkDestroyDevice(m_device, nullptr);
    vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    vkDestroyInstance(m_instance, nullptr);
    m_device = VK_NULL_HANDLE;
}

bool Device::beginFrame()
{
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
    return 0;
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
    if (!check(vkAllocateMemory(m_device, &allocate, nullptr, &buffer.memory), error, "vkAllocateMemory"))
        return buffer;
    vkBindBufferMemory(m_device, buffer.handle, buffer.memory, 0);

    if (hostVisible)
        vkMapMemory(m_device, buffer.memory, 0, size, 0, &buffer.mapped);
    return buffer;
}

void Device::destroyBuffer(Buffer &buffer)
{
    if (buffer.mapped)
        vkUnmapMemory(m_device, buffer.memory);
    if (buffer.handle)
        vkDestroyBuffer(m_device, buffer.handle, nullptr);
    if (buffer.memory)
        vkFreeMemory(m_device, buffer.memory, nullptr);
    buffer = {};
}

} // namespace render
