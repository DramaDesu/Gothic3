#pragma once

// Compiled static mesh (.xcmsh). One mesh element is one material and one draw
// call; its index buffer is local to the element, so elements are independent.

#include "property_set.h"

#include <array>
#include <string>
#include <vector>

namespace genome
{

// eEVertexStreamArrayType, the values that actually occur in shipping data.
enum class StreamType : std::uint32_t
{
    Face = 0,
    Position = 1,
    Normal = 3,
    Diffuse = 4,   // not a colour: the red byte carries bitangent handedness
    Specular = 5,
    TexCoord0 = 12,
    TexCoord1 = 15,
    TexCoord2 = 18,
    TexCoord3 = 21,
    Tangent = 64,
    LightmapUV = 73,
};

struct MeshElement
{
    std::string materialName;
    std::array<float, 3> boundsMin{};
    std::array<float, 3> boundsMax{};

    std::vector<std::uint32_t> indices;                  // triangle list, element-local
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> normals;
    std::vector<std::array<float, 3>> tangents;
    std::vector<std::array<float, 2>> texCoords;         // UV set 0
    std::vector<std::uint32_t> diffuse;                  // bitangent handedness, see above

    // Stream 5, on every element of every mesh in the archive - 9299 of 9299.
    // Packed four bytes per vertex, and zero in every one of them.
    std::vector<std::uint32_t> vertexLight;

    // Stream 73, on 2263 elements: the coordinates that address the baked
    // lightmap bitmaps rather than the diffuse texture.
    std::vector<std::array<float, 2>> lightmapUV;

    // Which streams this element actually carried, in file order. Kept because
    // the ones we skip are as interesting as the ones we read - a stream nobody
    // reads is a feature nobody draws.
    std::vector<std::uint32_t> streams;

    std::size_t triangleCount() const { return indices.size() / 3; }
};

struct Mesh
{
    std::string name;
    std::array<float, 3> boundsMin{};
    std::array<float, 3> boundsMax{};
    std::vector<MeshElement> elements;

    std::size_t vertexCount() const;
    std::size_t triangleCount() const;
};

// Parses a whole .xcmsh file. Returns false and fills `error` on malformed data.
bool loadMesh(const std::vector<std::uint8_t> &bytes, Mesh &mesh, std::string *error = nullptr);

} // namespace genome
