// Plays a motion on an actor and reports the skinned silhouette, which is the
// cheapest way to tell a correct pose from a plausible-looking wrong one.

#include "genome/motion.h"
#include "genome/pak.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace
{

struct Bounds
{
    std::array<float, 3> min{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max()};
    std::array<float, 3> max{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                             std::numeric_limits<float>::lowest()};

    void add(const std::array<float, 3> &point)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            min[axis] = std::min(min[axis], point[axis]);
            max[axis] = std::max(max[axis], point[axis]);
        }
    }

    void print(const char *label) const
    {
        std::printf("%-22s x %7.1f..%7.1f   y %7.1f..%7.1f   z %7.1f..%7.1f   (%.1f cm tall)\n", label, min[0], max[0],
                    min[1], max[1], min[2], max[2], max[1] - min[1]);
    }
};

Bounds boundsOf(const std::vector<std::array<float, 3>> &points)
{
    Bounds bounds;
    for (const auto &point : points)
        bounds.add(point);
    return bounds;
}

void reportBones(const genome::Actor &actor, const genome::Skeleton &skeleton, const genome::Motion &motion)
{
    const std::vector<genome::Matrix4> pose = genome::samplePose(skeleton, motion, 0.0f);
    const std::vector<genome::Matrix4> skinning = genome::skinningMatrices(actor, skeleton, pose);

    std::puts("first bones at t = 0 (pose position, then the skinning matrix translation):");
    for (std::size_t index = 0; index < 4 && index < skeleton.bones.size(); ++index)
    {
        const genome::Skeleton::Bone &bone = skeleton.bones[index];
        const genome::Matrix4 &skin = skinning[bone.originalNode];
        const genome::Matrix4 &bind = actor.nodes[bone.originalNode].globalBind;
        std::printf("  %-26s pose %8.2f %8.2f %8.2f | bind %8.2f %8.2f %8.2f | skin %8.2f %8.2f %8.2f\n",
                    bone.name.c_str(), pose[index][12], pose[index][13], pose[index][14], bind[12], bind[13], bind[14],
                    skin[12], skin[13], skin[14]);
    }
    std::printf("skinning lists %zu, first vertex influences %zu, vertices %zu\n", actor.influences.size(),
                actor.influences.empty() ? std::size_t(0) : actor.influences[0].size(), actor.vertexCount());
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::puts("usage: g3anim <archive.pak> <actor.xact> <motion.xmot> [time]");
        return 2;
    }

    std::string error;
    const auto archive = genome::PakArchive::open(argv[1], &error);
    if (!archive)
    {
        std::cerr << "error: " << error << "\n";
        return 1;
    }

    genome::Actor actor;
    if (!genome::loadActor(archive->read(argv[2], &error), actor, &error))
    {
        std::cerr << "actor: " << error << "\n";
        return 1;
    }

    genome::Motion motion;
    if (!genome::loadMotion(archive->read(argv[3], &error), motion, &error))
    {
        std::cerr << "motion: " << error << "\n";
        return 1;
    }

    const genome::Skeleton skeleton = genome::buildSkeleton(actor);
    std::size_t matched = 0;
    for (const genome::Skeleton::Bone &bone : skeleton.bones)
        matched += motion.find(bone.name) != nullptr ? 1 : 0;

    std::printf("actor    %zu nodes -> %zu bones after folding helpers\n", actor.nodes.size(), skeleton.bones.size());
    std::printf("motion   %zu parts, %zu matched to bones, %.2f s", motion.parts.size(), matched, motion.duration);
    if (!motion.effects.empty())
    {
        std::printf(", effects:");
        for (const auto &[time, name] : motion.effects)
            std::printf(" %.2fs %s", time, name.c_str());
    }
    std::puts("");

    std::array<float, 3> min{}, max{};
    actor.computeBounds(min, max);
    Bounds bind;
    bind.add(min);
    bind.add(max);
    bind.print("bind pose");

    if (const char *verbose = std::getenv("G3_VERBOSE"); verbose && *verbose == '1')
        reportBones(actor, skeleton, motion);

    const float requested = argc > 4 ? std::strtof(argv[4], nullptr) : -1.0f;
    std::vector<std::array<float, 3>> skinned;
    for (float time : {0.0f, motion.duration * 0.25f, motion.duration * 0.5f, motion.duration * 0.75f})
    {
        if (requested >= 0.0f)
            time = requested;
        const std::vector<genome::Matrix4> pose = genome::samplePose(skeleton, motion, time);
        const std::vector<genome::Matrix4> skinning = genome::skinningMatrices(actor, skeleton, pose);
        genome::skinVertices(actor, skinning, skinned);

        char label[64];
        std::snprintf(label, sizeof(label), "t = %.2f s", time);
        boundsOf(skinned).print(label);
        if (requested >= 0.0f)
            break;
    }
    return 0;
}
