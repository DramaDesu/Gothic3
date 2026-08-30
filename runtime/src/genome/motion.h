#pragma once

// Motion clips (.xmot) and the skeleton they actually address.
//
// A clip does not animate the actor's raw node tree: the engine first folds away
// helper nodes, and the clip's transforms are expressed against that cleaned
// hierarchy. Driving the raw tree instead misplaces bones by tens of
// centimetres, so the cleanup lives here next to the sampling code.

#include "actor.h"

#include <map>
#include <string>
#include <vector>

namespace genome
{

struct Transform
{
    std::array<float, 3> position{};
    std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 3> scale{1.0f, 1.0f, 1.0f};
};

// The actor hierarchy with helper nodes folded into their children, which is
// what motion parts are named against.
struct Skeleton
{
    struct Bone
    {
        std::string name;
        int parent = -1;
        int originalNode = -1; // index into Actor::nodes, for skinning
        Transform rest;
    };

    std::vector<Bone> bones;
    int find(std::string_view name) const;
};

Skeleton buildSkeleton(const Actor &actor);

struct MotionTrack
{
    char channel = 0; // 'P', 'R' or 'S'
    std::vector<float> times;
    std::vector<std::array<float, 4>> values; // rotation uses all four
};

struct MotionPart
{
    std::string name;
    Transform pose; // used when a channel has no track
    std::vector<MotionTrack> tracks;
};

struct Motion
{
    std::vector<MotionPart> parts;
    std::vector<std::pair<float, std::string>> effects; // seconds, effect name
    float duration = 0.0f;
    float frameRate = 25.0f;

    const MotionPart *find(std::string_view name) const;
};

bool loadMotion(const std::vector<std::uint8_t> &bytes, Motion &motion, std::string *error = nullptr);

// Samples the clip at `time` and returns one global matrix per skeleton bone.
std::vector<Matrix4> samplePose(const Skeleton &skeleton, const Motion &motion, float time);

// Skinning matrices ready for the GPU: global pose times inverse bind, indexed
// by the actor's own node numbering so vertex influences resolve directly.
//
// `actor` need not be the actor the skeleton came from. A character is
// assembled from several actors - a body, a head, whatever the slots carry -
// that share a skeleton only by bone NAME, each with its own node order and its
// own bind pose. Matching by name is what lets one pose drive all of them.
std::vector<Matrix4> skinningMatrices(const Actor &actor, const Skeleton &skeleton,
                                      const std::vector<Matrix4> &pose);

// Applies skinning on the CPU. Useful as a correctness check before any GPU
// work exists.
void skinVertices(const Actor &actor, const std::vector<Matrix4> &skinning, std::vector<std::array<float, 3>> &out);

} // namespace genome
