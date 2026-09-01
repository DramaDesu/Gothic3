#pragma once

// Baked lighting (.xlmp). One file per placed instance rather than per mesh -
// 11234 of them - which is why the meshes themselves carry none: they are shared
// between thousands of placements and the light is not.
//
// Each mesh element gets a colour and an incident direction per vertex. The
// direction is what a normal-mapped surface needs and a flat colour cannot give,
// and the colour's top byte is not alpha but ambient occlusion: the engine's
// bake writes it as the fraction of rays that reached the sky, and writes the
// light itself into the low 24 bits in a separate pass.

#include <cstdint>
#include <string>
#include <vector>

namespace genome
{

// Whether the instance is lit per vertex alone, or also by bitmaps.
enum class LightmapType : std::uint32_t
{
    PerVertex = 0,
    Mixed = 1,
};

// A patch of baked light as an image rather than per vertex, placed into the
// element's second UV set. Mixed instances carry these as well.
struct LightmapBitmap
{
    std::int32_t uvSet = 0;
    std::int32_t offsetX = 0, offsetY = 0;
    std::int32_t width = 0, height = 0;
    std::vector<std::uint8_t> data;
};

struct LightmapElement
{
    std::vector<std::uint32_t> colours;   // one per vertex: light in the low 24 bits, occlusion in the top byte
    std::vector<float> incident;          // three floats per vertex, a unit direction
    std::vector<LightmapBitmap> bitmaps;

    std::size_t vertexCount() const { return colours.size(); }
};

struct Lightmap
{
    std::string meshName;                 // the mesh this instance lights, in full
    float resourcePriority = 0.0f;
    float scaling = 0.0f;
    LightmapType type = LightmapType::PerVertex;

    // One per element of that mesh, in element order.
    std::vector<LightmapElement> elements;

    std::size_t vertexCount() const;
};

bool loadLightmap(const std::vector<std::uint8_t> &bytes, Lightmap &out, std::string *error);

} // namespace genome
