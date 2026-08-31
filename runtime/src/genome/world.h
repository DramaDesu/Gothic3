#pragma once

// World layers: the entity lists that place everything in the level.
//
// `.node` files hold the static geometry of one sector - the cell position is
// in the file name - while `.lrentdat` holds dynamic content. Both share the
// same container and the same entity record, differing only in their header.

#include "property_set.h"

#include <array>
#include <string>
#include <vector>

namespace genome
{

// Row-major, row-vector convention: the translation is the fourth row, which is
// how the engine stores it.
using WorldMatrix = std::array<float, 16>;

struct Placement
{
    std::string name;
    WorldMatrix world{};   // absolute, no parent to resolve
    std::string meshName;  // from eCVisualMeshStatic_PS, empty when not a mesh

    std::array<float, 3> translation() const { return {world[12], world[13], world[14]}; }
};

struct WorldLayer
{
    std::vector<Placement> placements;

    std::size_t meshCount() const;
};

// Parses a .node sector. Entities without a static mesh are kept, since their
// names and transforms still describe the level.
bool loadWorldNode(const std::vector<std::uint8_t> &bytes, WorldLayer &layer, std::string *error = nullptr);

} // namespace genome
