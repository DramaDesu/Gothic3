#pragma once

// Renders grown trees into one texture, so a distant tree can be drawn as a
// single quad instead of four thousand triangles.
//
// The game shipped billboards of its own and we cannot use them: the field that
// says which cell of its atlas belongs to which tree is left at the whole
// texture in 78 of the 98 definitions. Since we grow the trees ourselves, we can
// draw them ourselves - which also gives a billboard that matches our tree
// rather than the one SpeedTree would have grown.

#include "genome/image.h"
#include "texture.h"
#include "vulkan.h"

#include <array>
#include <string>
#include <vector>

namespace render
{

class WorldRenderer;

struct TreeAtlas
{
    // Where each baked tree ended up, as (u0, v0, u1, v1).
    std::vector<std::array<float, 4>> cells;
    Texture texture;
    std::uint32_t size = 0;

    bool valid() const { return texture.valid(); }
};

// Draws one batch of `source` per cell, from the side, with an orthographic
// camera fitted to that batch's mesh. `batches` names which batch of the
// renderer to draw into each cell, in order.
bool bakeTreeAtlas(Device &device, WorldRenderer &source, const std::vector<std::size_t> &batches,
                   std::uint32_t cellSize, TreeAtlas &out, std::string *error);

// Copies the baked atlas back into an ordinary image, so it goes through the
// same texture path as everything else the world draws.
bool readTreeAtlas(Device &device, const TreeAtlas &atlas, genome::Image &out, std::string *error);

void destroyTreeAtlas(Device &device, TreeAtlas &atlas);

} // namespace render
