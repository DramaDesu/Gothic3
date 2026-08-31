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

// A leaf kind: its texture and the sizes it is drawn at. Some trees have two.
struct LeafKind
{
    std::string texture;
    std::array<float, 3> size{};      // id 4004
    std::array<float, 3> sizeVariance{};
    std::array<float, 3> counts{};
    float scale = 0.0f;
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

struct SpeedTree
{
    std::string barkTexture;
    float height = 0.0f;             // id 2001, 50..1100 across the corpus
    float radius = 0.0f;             // id 2003
    std::uint32_t seed = 0;          // id 2005
    float parameter2006 = 0.0f;
    float parameter2007 = 0.0f;

    std::vector<BranchLevel> levels;
    std::vector<LeafKind> leaves;
    std::vector<SpeedTreeMaterial> materials;

    std::string billboardTexture;    // id 20002
    std::string shadowTexture;       // id 18005

    std::vector<SpeedTreeToken> tokens;   // everything, in file order
};

// Parses a .spt. Fails rather than guessing when an id is not in the width
// table, because an unknown width cannot be stepped over safely.
bool loadSpeedTree(const std::vector<std::uint8_t> &bytes, SpeedTree &out, std::string *error);

} // namespace genome
