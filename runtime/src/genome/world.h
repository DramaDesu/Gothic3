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

// One collision shape an entity declares. The engine's own list of kinds, and
// the two that name a file are the ones that carry the game's cooked geometry;
// the rest are primitives with their numbers in the record.
struct CollisionShape
{
    enum class Kind
    {
        None = 0,
        TriMesh = 1,
        Plane = 2,
        Box = 3,
        Capsule = 4,
        Sphere = 5,
        ConvexHull = 6,
        Point = 7,
    };

    Kind kind = Kind::None;

    // TriMesh and ConvexHull: the cooked file named outright, and which of its
    // sub-meshes this shape is. This is the authority the suffix rule stands in
    // for - a placement whose visual is a LOD set names a different mesh here
    // than appending "_col" to the visual's name would find.
    std::string meshName;
    std::uint16_t meshIndex = 0;

    // eEShapeGroup: what part of the thing this is. Trees have their own two -
    // trunk and branches - which is how the engine tells a trunk you walk into
    // from a canopy an arrow stops in.
    std::uint32_t group = 0;
    // eEShapeMaterial: wood, stone, foliage and the rest, in the order rmtools
    // documents for the material names inside a .xnvmsh.
    std::uint32_t material = 0;

    bool disableCollision = false;
    bool ignoredByTraceRay = false;

    // Box, Capsule and Sphere carry their numbers here instead of naming a
    // file, in the entity's own frame and in world units. The box and the
    // capsule are oriented; the rows of this apply the way the entity matrices'
    // rows do, and it is the identity for a sphere.
    std::array<float, 3> centre{};
    std::array<float, 3> extent{}; // box: half extents
    std::array<float, 9> orientation{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    float radius = 0.0f; // capsule and sphere
    float height = 0.0f; // capsule
};

struct Placement
{
    std::string name;

    // The instance's own identifier, which is how its baked lighting is found:
    // Lightmaps.pak names every file <mesh>_{guid}.xlmp, and those bytes are
    // this field.
    std::string guid;
    WorldMatrix world{};   // absolute, no parent to resolve
    std::string meshName;  // from eCVisualMeshStatic_PS, empty when not a mesh

    // World-space bounds the engine already computed for this placement, which
    // is exactly what visibility tests want.
    std::array<float, 3> boundsMin{};
    std::array<float, 3> boundsMax{};

    // What the entity says to collide with. Empty for most placements, which
    // is why the name rule is still needed.
    std::vector<CollisionShape> shapes;

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

// One entity out of a .lrentdat - an NPC, an item, an interactable. The static
// world is in the sectors; these are the things in it.
struct DynamicEntity
{
    std::string name;
    std::string guid;
    WorldMatrix world{};

    // eCVisualAnimation_PS: the .xact this one wears. Empty for the many
    // entities that are not drawn as characters - waypoints, spawn markers,
    // routine anchors.
    std::string actorName;

    // Which property sets it carries, in the order the file lists them. Kept
    // whole rather than reduced to flags: what an entity is, is mostly which of
    // these it has - gCNPC_PS and gCDialog_PS make a person, gCInventoryStack
    // an item lying on the ground.
    std::vector<std::string> classes;

    // Its place in the file's tree, which is how a routine point knows the
    // character it belongs to. -1 for the root.
    int parent = -1;

    std::array<float, 3> translation() const { return {world[12], world[13], world[14]}; }
    bool has(const std::string &className) const;
};

// Parses a .lrentdat. The entity tree is flattened: each entity records its
// parent's index rather than owning its children, since every use so far reads
// the list and looks up parents rather than walking down.
bool loadEntityFile(const std::vector<std::uint8_t> &bytes, std::vector<DynamicEntity> &out,
                    std::string *error = nullptr);

// Parses a .node sector. Entities without a static mesh are kept, since their
// names and transforms still describe the level.
bool loadWorldNode(const std::vector<std::uint8_t> &bytes, WorldLayer &layer, std::string *error = nullptr);

} // namespace genome
