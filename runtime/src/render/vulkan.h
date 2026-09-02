#pragma once

// Minimal Vulkan 1.3 setup: instance, device, swapchain and per-frame sync.
// Dynamic rendering is used throughout, so there are no render pass or
// framebuffer objects to carry around.

#include <cstdint>
#include <map>
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

    // What findMemoryType says when nothing matches. Not zero: zero is a real
    // memory type, and on this device it is host memory.
    static constexpr std::uint32_t c_NoMemoryType = 0xFFFFFFFFu;

    bool create(Window &window, std::string *error, bool validation = false);
    void destroy();

    // Returns false when the swapchain needs rebuilding; the caller should skip
    // the frame and try again.
    bool beginFrame();
    void endFrame();
    bool recreateSwapchain();

    VkDevice device() const { return m_device; }
    VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
    VkCommandBuffer commandBuffer() const { return m_commandBuffers[m_frame]; }
    VkQueue queue() const { return m_queue; }
    VkFormat colorFormat() const { return m_surfaceFormat.format; }
    VkFormat depthFormat() const { return m_depthFormat; }
    VkExtent2D extent() const { return m_extent; }
    std::uint32_t frameIndex() const { return m_frame; }

    // Frames begun since startup. Anything handed back to a pool has to wait
    // out the frames in flight before it can be handed out again, and this is
    // what that is counted in.
    std::uint64_t frameCounter() const { return m_frameCounter; }

    VkImageView currentColorView() const { return m_swapchainViews[m_imageIndex]; }
    VkImage currentColorImage() const { return m_swapchainImages[m_imageIndex]; }
    VkImageView depthView() const { return m_depthView; }

    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, std::string *error);
    void destroyBuffer(Buffer &buffer);

    bool allocate(const VkMemoryRequirements &requirements, bool hostVisible, VkDeviceMemory &memory,
                  std::string *error);

    // Everything allocated above comes back through here, so that what is live
    // is actually known rather than assumed.
    void freeAllocation(VkDeviceMemory &memory);

    // Records and runs a command buffer immediately, for uploads at load time.
    // Writes the frame that was just presented to a binary PPM. Screen grabs
    // need the window in front, which is rude and unreliable while the machine
    // is in use, so the runtime takes its own picture.
    bool capture(const char *path, std::string *error);

    // One sampler per texture is one Vulkan object per texture, and the driver
    // guarantees only 4000 of them - the game ships 1897 images and the number
    // was climbing. Every texture wants the same filtering, so they share, and
    // the device owns them rather than the textures.
    VkSampler sampler(bool clampToEdge);

    VkCommandBuffer beginOneShot();
    void endOneShot(VkCommandBuffer command);

    // Submits without waiting for anything. There is one queue, so this runs
    // before whatever is submitted after it, and the barrier below makes its
    // writes visible to those commands - so a copy for a sector that arrives
    // this frame is ready by the time this frame's draws read it, with no
    // drain. The buffers handed over are freed when the copy has really
    // finished, and the submission is fenced only for that.
    void endOneShotAsync(VkCommandBuffer command, const std::vector<Buffer> &recycle = {});

    // Records the dependency that makes the above legal: everything written by
    // transfers up to here is visible to every later command that reads a
    // vertex, an index, a storage buffer or a texture.
    void barrierAfterTransfer(VkCommandBuffer command);

    // Frees what finished. Called once a frame; safe to call at any time.
    void retireTransfers(bool waitForAll = false);

    // How many transfers are still in flight, and how many drains were paid to
    // keep that number down.
    std::size_t transfersInFlight() const { return m_retiring.size(); }
    std::size_t transferStalls() const { return m_transferStalls; }

    // Every image and every buffer takes a VkDeviceMemory of its own, and the
    // driver gives out a fixed number of them. Nothing here had ever asked how
    // many, which is how a wall stays invisible until you hit it.
    std::size_t liveAllocations() const { return m_liveAllocations; }
    std::size_t peakAllocations() const { return m_peakAllocations; }
    std::uint32_t allocationLimit() const { return m_allocationLimit; }
    // How many times the swapchain was rebuilt. Each one drains the device,
    // so a frame that took a quarter of a second is worth checking against it.
    std::size_t swapchainRebuilds() const { return m_swapchainRebuilds; }
    VkDeviceSize liveBytes() const { return m_liveBytes; }
    VkDeviceSize peakBytes() const { return m_peakBytes; }

  private:
    bool pickPhysicalDevice(std::string *error);
    bool createSwapchain(std::string *error);
    void destroySwapchain();
    bool createDepth(std::string *error);
    std::uint32_t findMemoryType(std::uint32_t mask, VkMemoryPropertyFlags properties) const;

    // Every vkAllocateMemory in the program goes past here. There are three of
    // them and they are easy to add a fourth to, which is exactly how the count
    // was wrong the first time.
    void noteAllocation(VkDeviceSize bytes);

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
    VkSampler m_repeatSampler = VK_NULL_HANDLE;
    VkSampler m_clampSampler = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;

    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkCommandBuffer m_commandBuffers[c_FramesInFlight]{};
    VkSemaphore m_acquired[c_FramesInFlight]{};
    VkSemaphore m_rendered[c_FramesInFlight]{};
    VkFence m_inFlight[c_FramesInFlight]{};

    // Transfers submitted and not yet known to be finished. Bounded, because a
    // load that submits hundreds of them would otherwise hold every staging
    // buffer alive at once.
    struct Retiring
    {
        VkFence fence = VK_NULL_HANDLE;
        VkCommandBuffer command = VK_NULL_HANDLE;
        std::vector<Buffer> recycle;
    };
    static constexpr std::size_t c_MaxTransfersInFlight = 8;
    std::vector<Retiring> m_retiring;
    std::vector<VkFence> m_spareFences;
    std::size_t m_transferStalls = 0;
    std::size_t m_liveAllocations = 0;
    std::size_t m_peakAllocations = 0;
    std::size_t m_swapchainRebuilds = 0;
    VkDeviceSize m_liveBytes = 0;
    VkDeviceSize m_peakBytes = 0;
    // What each allocation was worth, so giving it back subtracts the right
    // amount rather than an average.
    std::map<VkDeviceMemory, VkDeviceSize> m_allocationBytes;
    std::uint32_t m_allocationLimit = 0;

    std::uint32_t m_frame = 0;
    std::uint32_t m_imageIndex = 0;
    std::uint64_t m_frameCounter = 0;
};

} // namespace render
