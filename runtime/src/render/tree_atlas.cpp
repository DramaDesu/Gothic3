#include "tree_atlas.h"

#include "world_renderer.h"

#include <cmath>
#include <cstring>

namespace render
{
namespace
{

// An orthographic camera looking along -Z at a box, fitted so the box exactly
// fills the cell. The tree is drawn from the side because that is how it is seen
// from anywhere on the ground.
std::array<float, 16> fitBox(const std::array<float, 6> &box)
{
    const float centreX = 0.5f * (box[0] + box[3]);
    const float centreY = 0.5f * (box[1] + box[4]);
    const float centreZ = 0.5f * (box[2] + box[5]);

    // A square cell, so the wider of the two axes decides - a tree drawn to fill
    // the cell in one axis and not the other would come out stretched.
    const float halfWidth = 0.5f * std::max(box[3] - box[0], 0.001f);
    const float halfHeight = 0.5f * std::max(box[4] - box[1], 0.001f);
    const float half = std::max(halfWidth, halfHeight) * 1.02f;
    const float depth = std::max(box[5] - box[2], 1.0f) * 4.0f;

    std::array<float, 16> m{};
    m[0] = 1.0f / half;
    m[5] = -1.0f / half; // Vulkan's clip space has Y downwards
    m[10] = -1.0f / depth;
    m[12] = -centreX / half;
    m[13] = centreY / half;
    m[14] = 0.5f - centreZ / depth;
    m[15] = 1.0f;
    return m;
}

void transition(VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to,
                VkPipelineStageFlags fromStage, VkPipelineStageFlags toStage, VkAccessFlags fromAccess,
                VkAccessFlags toAccess, VkImageAspectFlags aspect)
{
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.image = image;
    barrier.subresourceRange = {aspect, 0, 1, 0, 1};
    barrier.srcAccessMask = fromAccess;
    barrier.dstAccessMask = toAccess;
    vkCmdPipelineBarrier(command, fromStage, toStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

bool createImage(Device &device, std::uint32_t size, VkFormat format, VkImageUsageFlags usage,
                 VkImageAspectFlags aspect, VkImage &image, VkDeviceMemory &memory, VkImageView &view,
                 std::string *error)
{
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {size, size, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device.device(), &info, nullptr, &image) != VK_SUCCESS)
    {
        if (error)
            *error = "vkCreateImage failed for the tree atlas";
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device.device(), image, &requirements);
    if (!device.allocate(requirements, false, memory, error))
        return false;
    vkBindImageMemory(device.device(), image, memory, 0);

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCreateImageView(device.device(), &viewInfo, nullptr, &view);
    return true;
}

} // namespace

bool bakeTreeAtlas(Device &device, WorldRenderer &source, const std::vector<std::size_t> &batches,
                   std::uint32_t cellSize, TreeAtlas &out, std::string *error)
{
    if (batches.empty())
        return true;

    // A square grid big enough for all of them, rounded up to a power of two so
    // the texture stays a comfortable shape.
    std::uint32_t columns = 1;
    while (columns * columns < batches.size())
        ++columns;
    std::uint32_t size = 1;
    while (size < columns * cellSize)
        size *= 2;

    out.size = size;
    out.cells.clear();

    // The pipeline that draws into this was built for the swapchain's format,
    // and a mismatch here is undefined behaviour that happens to work - which is
    // exactly what the validation layers said when they were first switched on.
    const VkFormat format = device.colorFormat();
    if (!createImage(device, size, format,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, out.texture.image, out.texture.memory, out.texture.view, error))
        return false;

    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
    if (!createImage(device, size, device.depthFormat(), VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT, depthImage, depthMemory, depthView, error))
        return false;

    // Clamped rather than wrapped: a cell must not bleed into its neighbour.
    // The device owns it, shared with anything else that clamps.
    out.texture.sampler = device.sampler(true);

    source.prepareAll(device);

    VkCommandBuffer command = device.beginOneShot();

    transition(command, out.texture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    transition(command, depthImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0,
               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = out.texture.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = depthView;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, {size, size}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    rendering.pDepthAttachment = &depth;
    vkCmdBeginRendering(command, &rendering);

    for (std::size_t index = 0; index < batches.size(); ++index)
    {
        const std::uint32_t column = std::uint32_t(index) % columns;
        const std::uint32_t row = std::uint32_t(index) / columns;
        const float x = float(column * cellSize);
        const float y = float(row * cellSize);

        VkViewport viewport{x, y, float(cellSize), float(cellSize), 0.0f, 1.0f};
        VkRect2D scissor{{std::int32_t(x), std::int32_t(y)}, {cellSize, cellSize}};
        vkCmdSetViewport(command, 0, 1, &viewport);
        vkCmdSetScissor(command, 0, 1, &scissor);

        const std::array<float, 6> *box = source.batchExtent(batches[index]);
        static const std::array<float, 6> c_Unit{-1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
        source.drawBatch(device, batches[index], fitBox(box ? *box : c_Unit), command);

        const float scale = 1.0f / float(size);
        out.cells.push_back({x * scale, y * scale, (x + float(cellSize)) * scale, (y + float(cellSize)) * scale});
    }

    vkCmdEndRendering(command);

    transition(command, out.texture.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    device.endOneShot(command);

    vkDestroyImageView(device.device(), depthView, nullptr);
    vkDestroyImage(device.device(), depthImage, nullptr);
    device.freeAllocation(depthMemory);
    return true;
}

bool readTreeAtlas(Device &device, const TreeAtlas &atlas, genome::Image &out, std::string *error)
{
    if (!atlas.valid() || atlas.size == 0)
        return false;

    const VkDeviceSize bytes = VkDeviceSize(atlas.size) * atlas.size * 4;
    Buffer staging = device.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, error);
    if (!staging.handle)
        return false;

    VkCommandBuffer command = device.beginOneShot();
    transition(command, atlas.texture.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
               VK_IMAGE_ASPECT_COLOR_BIT);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {atlas.size, atlas.size, 1};
    vkCmdCopyImageToBuffer(command, atlas.texture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.handle, 1,
                           &region);

    transition(command, atlas.texture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_IMAGE_ASPECT_COLOR_BIT);
    device.endOneShot(command);

    out = genome::Image{};
    out.width = atlas.size;
    out.height = atlas.size;
    out.faceCount = 1;
    out.format = genome::ImageFormat::A8R8G8B8;
    out.data.resize(std::size_t(bytes));

    // The swapchain format is blue first, which is the order the loader's
    // A8R8G8B8 already stores, so this is a straight copy.
    std::memcpy(out.data.data(), staging.mapped, out.data.size());
    out.levels.push_back({atlas.size, atlas.size, 0, std::uint32_t(bytes)});
    out.faceStride = std::uint32_t(bytes);

    device.destroyBuffer(staging);
    return true;
}

void destroyTreeAtlas(Device &device, TreeAtlas &atlas)
{
    destroyTexture(device, atlas.texture);
    atlas.cells.clear();
}

} // namespace render
