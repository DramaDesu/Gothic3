// Diagnostic: what does a clip actually drive, and where does our rest pose
// disagree with the reference pose the clip carries for the same bone?

#include "genome/motion.h"
#include "genome/pak.h"

#include <cmath>
#include <cstdio>
#include <iostream>

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::puts("usage: g3probe <archive.pak> <actor.xact> <motion.xmot>");
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
    genome::Motion motion;
    if (!genome::loadActor(archive->read(argv[2], &error), actor, &error) ||
        !genome::loadMotion(archive->read(argv[3], &error), motion, &error))
    {
        std::cerr << "error: " << error << "\n";
        return 1;
    }

    const genome::Skeleton skeleton = genome::buildSkeleton(actor);

    std::puts("parts carrying a position track:");
    int positionTracks = 0;
    for (const genome::MotionPart &part : motion.parts)
    {
        for (const genome::MotionTrack &track : part.tracks)
        {
            if (track.channel != 'P')
                continue;
            ++positionTracks;
            std::printf("  %-28s %3zu keys, first %8.2f %8.2f %8.2f\n", part.name.c_str(), track.times.size(),
                        track.values.front()[0], track.values.front()[1], track.values.front()[2]);
        }
    }
    std::printf("%d position tracks out of %zu parts\n\n", positionTracks, motion.parts.size());

    std::puts("bones whose rest position differs from the clip's reference pose:");
    for (const genome::Skeleton::Bone &bone : skeleton.bones)
    {
        const genome::MotionPart *part = motion.find(bone.name);
        if (!part)
            continue;

        double distance = 0.0;
        for (int axis = 0; axis < 3; ++axis)
        {
            const double delta = double(bone.rest.position[axis]) - part->pose.position[axis];
            distance += delta * delta;
        }
        distance = std::sqrt(distance);
        if (distance < 0.5)
            continue;

        bool hasPositionTrack = false;
        for (const genome::MotionTrack &track : part->tracks)
            hasPositionTrack = hasPositionTrack || track.channel == 'P';

        std::printf("  %-28s %7.2f cm apart, position track: %s\n", bone.name.c_str(), distance,
                    hasPositionTrack ? "yes" : "NO - our rest is being overwritten by the reference pose");
        std::printf("      rest %8.2f %8.2f %8.2f\n", bone.rest.position[0], bone.rest.position[1],
                    bone.rest.position[2]);
        std::printf("      pose %8.2f %8.2f %8.2f\n", part->pose.position[0], part->pose.position[1],
                    part->pose.position[2]);
    }
    std::puts("\nvertices bound to bones the clip never drives:");
    std::vector<std::size_t> weightPerNode(actor.nodes.size(), 0);
    for (const auto &list : actor.influences)
        for (const genome::SkinInfluence &influence : list)
            if (influence.node < weightPerNode.size() && influence.weight > 0.01f)
                ++weightPerNode[influence.node];

    std::size_t undriven = 0;
    for (std::size_t node = 0; node < actor.nodes.size(); ++node)
    {
        if (weightPerNode[node] == 0)
            continue;
        const bool driven = motion.find(actor.nodes[node].name) != nullptr;
        if (driven)
            continue;
        undriven += weightPerNode[node];
        std::printf("  %-28s %6zu vertices, %s\n", actor.nodes[node].name.c_str(), weightPerNode[node],
                    skeleton.find(actor.nodes[node].name) >= 0 ? "in the skeleton" : "FOLDED AWAY as a helper");
    }
    std::printf("%zu weighted vertex references land on bones the clip does not animate\n", undriven);
    return 0;
}
