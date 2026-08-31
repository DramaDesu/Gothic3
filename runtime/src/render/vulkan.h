#pragma once

// Minimal Vulkan 1.3 setup: instance, device, swapchain and per-frame sync.
// Dynamic rendering is used throughout, so there are no render pass or
// framebuffer objects to carry around.

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace render
{

class Window;

struct Buffer
{
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = nullptr;
    VkDeviceSize size = 0;
};

class Device
{
  public:
    static constexpr std::uint32_t c_FramesInFlight = 2;

    bool create(Window &window, std::string *error);
    void destroy();

    // Returns false when the swapchain needs rebuilding; the caller should skip
    // the frame and try again.
    bool beginFrame();
    void endFrame();
    bool recreateSwapchain();

    VkDevice device() const { return m_device; }
    VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
    VkCommandBuffer commandBuffer() const { return m_commandBuffers[m_frame]; }
    VkFormat colorFormat() const { return m_surfaceFormat.format; }
    VkFormat depthFormat() const { return m_depthFormat; }
    VkExtent2D extent() const { return m_extent; }
    std::uint32_t frameIndex() const { return m_frame; }

    VkImageView currentColorView() const { return m_swapchainViews[m_imageIndex]; }
    VkImage currentColorImage() const { return m_swapchainImages[m_imageIndex]; }
    VkImageView depthView() const { return m_depthView; }

    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, std::string *error);
    void destroyBuffer(Buffer &buffer);

    bool allocate(const VkMemoryRequirements &requirements, bool hostVisible, VkDeviceMemory &memory,
                  std::string *error);

    // Records and runs a command buffer immediately, for uploads at load time.
    // Writes the frame that was just presented to a binary PPM. Screen grabs
    // need the window in front, which is rude and unreliable while the machine
    // is in use, so the runtime takes its own picture.
    bool capture(const char *path, std::string *error);

    VkCommandBuffer beginOneShot();
    void endOneShot(VkCommandBuffer command);

  private:
    bool pickPhysicalDevice(std::string *error);
    bool createSwapchain(std::string *error);
    void destroySwapchain();
    bool createDepth(std::string *error);
    std::uint32_t findMemoryType(std::uint32_t mask, VkMemoryPropertyFlags properties) const;

    Window *m_window = nullptr;
    VkInstance m_instance = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    std::uint32_t m_queueFamily = 0;

    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkSurfaceFormatKHR m_surfaceFormat{};
    VkExtent2D m_extent{};
    std::vector<VkImage> m_swapchainImages;
    std::vector<VkImageView> m_swapchainViews;

    VkImage m_depthImage = VK_NULL_HANDLE;
    VkDeviceMemory m_depthMemory = VK_NULL_HANDLE;
    VkImageView m_depthView = VK_NULL_HANDLE;
    VkFormat m_depthFormat = VK_FORMAT_D32_SFLOAT;

    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkCommandBuffer m_commandBuffers[c_FramesInFlight]{};
    VkSemaphore m_acquired[c_FramesInFlight]{};
    VkSemaphore m_rendered[c_FramesInFlight]{};
    VkFence m_inFlight[c_FramesInFlight]{};

    std::uint32_t m_frame = 0;
    std::uint32_t m_imageIndex = 0;
};

} // namespace render
