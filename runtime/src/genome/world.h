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

// A SpeedTree instance. The sector only names a definition and gives the
// transform - the tree itself is grown from the .spt, and its size variance is
// applied per instance, which is why two entities sharing one definition have
// bounds of different heights.
struct TreePlacement
{
    std::string name;
    WorldMatrix world{};
    std::string resource; // e.g. "G3_Tree_XS_CarolinaBuckthorn_03.spt"
    std::array<float, 3> boundsMin{};
    std::array<float, 3> boundsMax{};
    bool wind = false;

    float boundsHeight() const { return boundsMax[1] - boundsMin[1]; }
};

// A static point light. The colour is three floats behind a four-byte tag, the
// range is in world units - a thousand of them is ten metres - and the offset
// moves the light off the entity's own origin, which is how a lamp hangs below
// the bracket that carries it.
struct PointLight
{
    std::array<float, 3> position{};
    std::array<float, 3> colour{1.0f, 1.0f, 1.0f};
    float range = 0.0f;
    bool castShadows = false;
};

struct WorldLayer
{
    std::vector<Placement> placements;
    std::vector<TreePlacement> trees;
    std::vector<PointLight> lights;
    std::vector<VegetationMesh> vegetationMeshes;
    std::vector<VegetationInstance> vegetation;

    std::size_t meshCount() const;
};

// Parses a .node sector. Entities without a static mesh are kept, since their
// names and transforms still describe the level.
bool loadWorldNode(const std::vector<std::uint8_t> &bytes, WorldLayer &layer, std::string *error = nullptr);

} // namespace genome
