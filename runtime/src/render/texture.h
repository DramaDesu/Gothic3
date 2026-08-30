#pragma once

// GPU textures built from Genome images. Block-compressed data goes to the GPU
// as it is - BC1/2/3 are exactly what the game shipped - so nothing is
// decompressed on the way.

#include "genome/image.h"
#include "vulkan.h"

#include <string>

namespace render
{

struct Texture
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;

    bool valid() const { return view != VK_NULL_HANDLE; }
};

// Uploads every mip level. `srgb` should be true for colour maps and false for
// data maps such as normals.
bool createTexture(Device &device, const genome::Image &source, bool srgb, Texture &texture, std::string *error);

void destroyTexture(Device &device, Texture &texture);

} // namespace render
