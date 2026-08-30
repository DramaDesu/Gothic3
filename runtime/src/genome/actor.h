#pragma once

// Animated actor (.xact): an EmotionFX-2 chunk stream carrying the skeleton,
// the skinned mesh and its weights. For a character this one file is enough -
// the body actor already contains every bone the master skeleton has.

#include "property_set.h"

#include <array>
#include <string>
#include <vector>

namespace genome
{

using Matrix4 = std::array<float, 16>; // column-major, like the GPU wants it

struct Node
{
    std::string name;
    std::string parentName;
    int parent = -1; // resolved by name; -1 for a root

    std::array<float, 3> position{};
    std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f}; // x, y, z, w
    std::array<float, 3> scale{1.0f, 1.0f, 1.0f};

    Matrix4 local{};      // from the components above
    Matrix4 globalBind{}; // parent-composed
    Matrix4 inverseBind{};
};

struct SkinInfluence
{
    std::uint16_t node = 0;
    float weight = 0.0f;
};

struct ActorVertex
{
    std::uint32_t originalVertex = 0; // index into the skinning table
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<float, 2> texCoord{};
};

struct ActorSubmesh
{
    std::uint8_t materialIndex = 0;
    std::vector<ActorVertex> vertices;
    std::vector<std::uint32_t> indices; // local to this submesh
};

struct Actor
{
    std::vector<Node> nodes;
    std::vector<ActorSubmesh> submeshes;

    // One entry per original (pre-split) vertex; a rendered vertex finds its
    // influences through ActorVertex::originalVertex.
    std::vector<std::vector<SkinInfluence>> influences;

    std::uint32_t meshNode = 0;
    std::vector<std::string> materials; // resource names, indexed by materialIndex

    std::size_t vertexCount() const;
    std::size_t triangleCount() const;
    const Node *findNode(std::string_view name) const;

    // Model-space bounds of the bind pose, recomputed: the stored box is invalid
    // in shipping files.
    void computeBounds(std::array<float, 3> &min, std::array<float, 3> &max) const;
};

bool loadActor(const std::vector<std::uint8_t> &bytes, Actor &actor, std::string *error = nullptr);

} // namespace genome
