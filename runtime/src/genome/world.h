#pragma once

// World layers: the entity lists that place everything in the level.
//
// `.node` files hold the static geometry of one sector - the cell position is
// in the file name - while `.lrentdat` holds dynamic content. Both share the
// same container and the same entity record, differing only in their header.

#include "property_set.h"

#include <array>
#include <cstdint>
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

    // World-space bounds the engine already computed for this placement, which
    // is exactly what visibility tests want.
    std::array<float, 3> boundsMin{};
    std::array<float, 3> boundsMax{};

    // The engine's own hints for when this object stops being worth drawing.
    float visualLodFactor = 1.0f;
    float objectCullFactor = 1.0f;

    std::array<float, 3> translation() const { return {world[12], world[13], world[14]}; }
};

// Grass and undergrowth. A sector stores one mesh per plant kind and a grid
// that scatters them, so the geometry here is a template and the instances
// come from VegetationInstance below.
struct VegetationMesh
{
    std::string texture; // authored name, e.g. "..._Diffuse_S1.dds"
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> normals;
    std::vector<std::array<float, 2>> texCoords;
    std::vector<std::uint32_t> indices;
    std::array<float, 3> boundsMin{};
    std::array<float, 3> boundsMax{};
};

// One plant: which mesh, and where it stands. Positions are already in world
// space, so the placing entity contributes nothing.
struct VegetationInstance
{
    std::uint32_t mesh = 0;
    WorldMatrix world{};
    std::array<float, 3> boundsMin{};
    std::array<float, 3> boundsMax{};
};

struct WorldLayer
{
    std::vector<Placement> placements;
    std::vector<VegetationMesh> vegetationMeshes;
    std::vector<VegetationInstance> vegetation;

    std::size_t meshCount() const;
};

// Parses a .node sector. Entities without a static mesh are kept, since their
// names and transforms still describe the level.
bool loadWorldNode(const std::vector<std::uint8_t> &bytes, WorldLayer &layer, std::string *error = nullptr);

} // namespace genome
