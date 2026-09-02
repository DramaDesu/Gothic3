#pragma once

// GPU textures built from Genome images. Block-compressed data goes to the GPU
// as it is - BC1/2/3 are exactly what the game shipped - so nothing is
// decompressed on the way.

#include "genome/image.h"
#include "vulkan.h"

#include <string>
#include <vector>

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

// One rectangle of the top level, and its own pixels.
struct TextureRegion
{
    std::uint32_t x = 0, y = 0, width = 0, height = 0;
    const void *bgra = nullptr;
};

// Replaces rectangles of the top level. The baked-patch atlas is written this
// way as sectors arrive: the image and its descriptor never change, only the
// texels inside tiles nothing is reading.
//
// Takes them together rather than one at a time because a submit costs a drain
// of the queue, and a sector brings about ten tiles.
bool updateTextureRegions(Device &device, Texture &texture, const std::vector<TextureRegion> &regions,
                          std::string *error);

void destroyTexture(Device &device, Texture &texture);

} // namespace render
