#include "texture.h"

#include <cstring>

namespace render
{
namespace
{

// The shipped formats map straight onto block-compressed Vulkan formats; the
// one uncompressed format in the data is BGRA.
VkFormat toVulkan(genome::ImageFormat format, bool srgb)
{
    switch (format)
    {
        case genome::ImageFormat::Dxt1: return srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case genome::ImageFormat::Dxt2:
        case genome::ImageFormat::Dxt3: return srgb ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK;
        case genome::ImageFormat::Dxt4:
        case genome::ImageFormat::Dxt5: return srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
        case genome::ImageFormat::A8R8G8B8:
        case genome::ImageFormat::X8R8G8B8:
            return srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
        default: return VK_FORMAT_UNDEFINED;
    }
}

} // namespace

bool createTexture(Device &device, const genome::Image &source, bool srgb, Texture &texture, std::string *error)
{
    const VkFormat format = toVulkan(source.format, srgb);
    if (format == VK_FORMAT_UNDEFINED)
    {
        if (error)
            *error = std::string("unsupported texture format ") + genome::formatName(source.format);
        return false;
    }

    const std::uint32_t mipCount = source.mipCount();

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {source.width, source.height, 1};
    imageInfo.mipLevels = mipCount;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device.device(), &imageInfo, nullptr, &texture.image) != VK_SUCCESS)
    {
        if (error)
            *error = "vkCreateImage failed";
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device.device(), texture.image, &requirements);
    if (!device.allocate(requirements, false, texture.memory, error))
        return false;
    vkBindImageMemory(device.device(), texture.image, texture.memory, 0);

    // One staging buffer for the whole chain, then a copy per level.
    Buffer staging = device.createBuffer(source.data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, error);
    if (!staging.handle)
        return false;
    std::memcpy(staging.mapped, source.data.data(), source.data.size());

    VkCommandBuffer command = device.beginOneShot();

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.image = texture.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipCount, 0, 1};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);

    std::vector<VkBufferImageCopy> copies;
    copies.reserve(mipCount);
    for (std::uint32_t mip = 0; mip < mipCount; ++mip)
    {
        const genome::ImageLevel level = source.level(mip, 0);
        VkBufferImageCopy copy{};
        copy.bufferOffset = level.offset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
        copy.imageExtent = {level.width, level.height, 1};
        copies.push_back(copy);
    }
    vkCmdCopyBufferToImage(command, staging.handle, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<std::uint32_t>(copies.size()), copies.data());

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);

    // Not waited for. The layout transition is in this command buffer, and one
    // queue means it has happened before anything submitted later samples it.
    device.endOneShotAsync(command, {staging});

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = texture.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipCount, 0, 1};
    vkCreateImageView(device.device(), &viewInfo, nullptr, &texture.view);

    // The game's UVs run far outside [0,1], so wrapping is not optional. The
    // sampler belongs to the device and is shared: it is the same for every
    // texture, and there is a hard limit on how many may exist.
    texture.sampler = device.sampler(false);

    return true;
}

bool updateTextureRegions(Device &device, Texture &texture, const std::vector<TextureRegion> &regions,
                          std::string *error)
{
    VkDeviceSize bytes = 0;
    for (const TextureRegion &region : regions)
        bytes += VkDeviceSize(region.width) * region.height * 4;
    if (bytes == 0)
        return true;

    Buffer staging = device.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, error);
    if (!staging.handle)
        return false;

    std::vector<VkBufferImageCopy> copies;
    copies.reserve(regions.size());
    VkDeviceSize at = 0;
    for (const TextureRegion &region : regions)
    {
        const VkDeviceSize size = VkDeviceSize(region.width) * region.height * 4;
        if (size == 0)
            continue;
        std::memcpy(static_cast<std::uint8_t *>(staging.mapped) + at, region.bgra, std::size_t(size));

        VkBufferImageCopy copy{};
        copy.bufferOffset = at;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageOffset = {std::int32_t(region.x), std::int32_t(region.y), 0};
        copy.imageExtent = {region.width, region.height, 1};
        copies.push_back(copy);
        at += size;
    }

    VkCommandBuffer command = device.beginOneShot();

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.image = texture.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);

    vkCmdCopyBufferToImage(command, staging.handle, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           std::uint32_t(copies.size()), copies.data());


    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);

    device.endOneShotAsync(command, {staging});
    return true;
}

void destroyTexture(Device &device, Texture &texture)
{
    // The sampler is the device's, shared by every texture, so it is not
    // destroyed here.
    if (texture.view)
        vkDestroyImageView(device.device(), texture.view, nullptr);
    if (texture.image)
        vkDestroyImage(device.device(), texture.image, nullptr);
    if (texture.memory)
        device.freeAllocation(texture.memory);
    texture = {};
}

} // namespace render
