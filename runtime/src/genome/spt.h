#pragma once

// SpeedTree definition (.spt). Not geometry: a parameter set the tree is grown
// from at runtime, which is why 98 trees fit in 600 KB.
//
// The file is a flat stream of tokens - a u32 id, then a value whose width is
// fixed per id and is written nowhere in the file. The original reader knew the
// widths from a table, so we carry that table too; see spt.cpp. Getting one
// width wrong does not fail, it desynchronises, and the stream then decodes into
// plausible nonsense - so the reader refuses ids it does not know rather than
// stepping over them.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace genome
{

// A profile curve. The control points describe a normalised shape and the header
// says what range it is drawn across, with a variance the generator applies per
// tree so two firs from one definition are not identical.
struct Spline
{
    struct Point
    {
        float t = 0.0f;         // position along the curve, always 0 at the first point and 1 at the last
        float value = 0.0f;
        float tangentX = 0.0f;  // (tangentX, tangentY) is a unit vector: verified on all 3913 points
        float tangentY = 0.0f;
        float tangentLength = 0.0f;
    };

    float low = 0.0f;
    float high = 0.0f;
    float variance = 0.0f;
    std::vector<Point> points;

    // The shape at t, before low/high are applied.
    float shape(float t) const;

    // The value the generator wants: the shape mapped onto low..high.
    float at(float t) const;
};

// One level of branching. A tree has four of these in shipping data - trunk,
// then three generations of branches - each with its own profile curves.
struct BranchLevel
{
    Spline profiles[9]{};             // ids 6000-6007 and 6017, in that order
    std::array<float, 7> numbers{};   // ids 6008-6014, whose meaning is still being pinned down
    std::uint8_t flags[2]{};          // ids 6015, 6016
};

// A leaf kind. The texture it names - "RedOakLeaves_RT_1.tga" - is not in the
// game archives: leaves are drawn from a shared composite atlas instead, and the
// corners below say which tile of it this kind uses.
struct LeafKind
{
    std::string texture;
    std::array<float, 3> pivot{};     // id 4004
    std::array<float, 3> variance{};  // id 4005
    std::array<float, 3> size{};      // id 4006, in the same units as the tree size
    float scale = 0.0f;               // id 4002
    std::array<float, 8> corners{};   // four (u, v) pairs into the composite atlas
    bool hasCorners = false;
};

// Fronds: flat blades that stand in for finely divided foliage. Conifers and
// palms carry their needles this way, where a leaf card would be wrong. Like the
// leaves, the texture named here is not in the archives - a frond is a tile of
// the same composite atlas.
struct Frond
{
    std::string texture;             // id 14002
    float scale = 0.0f;              // id 14003
    float width = 0.0f;              // id 14004
    float firstAngle = 0.0f;         // id 14005, degrees
    float secondAngle = 0.0f;        // id 14006, degrees
    std::uint32_t blades = 1;        // id 14007, one to five
    std::uint32_t level = 1;         // id 14008
    std::array<float, 8> corners{};
    bool hasCorners = false;
};

// Ambient, diffuse, specular and emissive colour plus a shininess - the shape of
// the three 52-byte records the format carries.
struct SpeedTreeMaterial
{
    std::array<float, 3> ambient{};
    std::array<float, 3> diffuse{};
    std::array<float, 3> specular{};
    std::array<float, 3> emissive{};
    float shininess = 0.0f;
};

// A token the reader decoded but does not yet have a name for. Keeping them lets
// a tool print the whole file, which is how the remaining ids get identified,
// without pretending the named set is complete.
struct SpeedTreeToken
{
    std::uint32_t id = 0;
    std::size_t offset = 0;
    std::vector<float> floats;   // for fixed-width numeric values
    std::string text;            // for string values
};

// A collision shape the definition declares. SpeedTree offers three kinds and
// Gothic 3 uses all three: a cylinder for the trunk and spheres or a box for the
// canopy. Every one of the 98 shipping definitions carries at least one.
//
// The numbers are in metres, in SpeedTree's own z-up frame - so the centre's
// third component is the height above the base, and it is what the height
// estimate below was already being fitted against.
struct CollisionPrimitive
{
    enum class Kind
    {
        Sphere,   // id 12002: centre and radius
        Cylinder, // id 12003: centre, radius and height, standing on the centre
        Box,      // id 12004: centre and half extents
    };

    Kind kind = Kind::Sphere;
    std::array<float, 3> centre{};
    float radius = 0.0f;          // sphere and cylinder
    float height = 0.0f;          // cylinder
    std::array<float, 3> extent{}; // box

    // The trunk is the one a player walks into; the canopy shapes are what a
    // thrown thing or an arrow hits.
    bool isTrunk() const { return kind == Kind::Cylinder; }
};

struct SpeedTree
{
    std::string barkTexture;
    // The distances at which the tree changes detail. They are constant within a
    // species, which is why 2001 looked like a height for so long - a red oak
    // and all its size variants carry 1100. The far one is greater than the near
    // one in all 98 files.
    float lodFarDistance = 0.0f;     // id 2001
    float lodNearDistance = 0.0f;    // id 2003
    std::uint32_t seed = 0;          // id 2005

    // Ids 3000-3010 are the leaf group, not the trunk. Two of them are worth
    // having: a site gets a leaf only if the running scale clears 3000, and then
    // only with probability 3002 - which is 0.05 for the umbrella thorns, 0.1
    // for the sparse firs and 1.0 for a red oak, in that order.
    float leafMinScale = 0.0f;       // id 3000
    float leafProbability = 1.0f;    // id 3002
    // The height the definition records for itself, in world units. Size alone
    // predicts the game's own tree heights poorly - a palm and an acacia of the
    // same size are nothing alike - but the 12000 band carries an extent that
    // does, and it is the yardstick the grown tree is scaled to.
    float recordedHeight = 0.0f;

    float size = 0.0f;               // id 2006
    float sizeVariance = 0.0f;       // id 2007

    std::vector<BranchLevel> levels;
    std::vector<LeafKind> leaves;
    Frond frond;
    std::vector<SpeedTreeMaterial> materials;

    std::string billboardTexture;    // id 20002, the composite atlas

    // The billboard quad: four (u, v) corners into that atlas, from id 20005.
    // Shipping data leaves it at the whole texture in 78 of the 98 files, so it
    // is read but cannot be trusted as a tile.
    std::array<float, 8> billboardCorners{};
    bool billboardCornersAuthored = false;
    std::string shadowTexture;       // id 18005

    // What the definition says to collide with, in metres and z-up.
    std::vector<CollisionPrimitive> collision;

    std::vector<SpeedTreeToken> tokens;   // everything, in file order
};

// Parses a .spt. Fails rather than guessing when an id is not in the width
// table, because an unknown width cannot be stepped over safely.
bool loadSpeedTree(const std::vector<std::uint8_t> &bytes, SpeedTree &out, std::string *error);

} // namespace genome
