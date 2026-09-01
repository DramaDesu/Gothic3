#pragma once

// Grows a mesh from a SpeedTree definition. The shipping game did this at load
// time with a library we do not have, so this is our own reading of the same
// parameters: the definition says how a tree of that species is put together,
// and a seed decides which particular tree comes out.

#include "mesh.h"
#include "spt.h"

#include <cstdint>

namespace genome
{

struct TreeGrowth
{
    // A definition carries a size and a variance; the runtime picks per instance
    // inside that range, which is why two trees from one file differ. Pass the
    // size you want, or leave it at zero to take the definition's own.
    float size = 0.0f;

    // How many children a level may spawn, whatever the definition asks for.
    // Shipping definitions ask for up to a thousand at the leaf level, and this
    // keeps a first draw honest about what it is drawing.
    // Instances vary; a definition compared against the game should not. With
    // this off the tree comes out at the size the file names, which is what the
    // game's mean height over thousands of instances can be measured against.
    bool applyVariance = true;

    // How much of the tree to build, for drawing it at a distance. Below one it
    // thins the foliage and stops one level short, which is what the definition's
    // own detail ladder does: fewer leaves, and bigger ones to cover for them.
    float detail = 1.0f;

    std::uint32_t branchLimit = 400;
    std::uint32_t leafLimit = 6000;
};

// The mesh comes out with two elements: bark, then leaves. Leaves are crossed
// cards, so they need the alpha test the grass already uses.
bool growTree(const SpeedTree &definition, std::uint32_t seed, const TreeGrowth &growth, Mesh &out);

} // namespace genome
