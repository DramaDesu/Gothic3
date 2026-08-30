// Dumps global bone positions at a given time so an independent decoder can be
// diffed against us.

#include "genome/motion.h"
#include "genome/pak.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>

int main(int argc, char **argv)
{
    if (argc < 5)
    {
        std::puts("usage: g3pose <archive.pak> <actor.xact> <motion.xmot> <time seconds>");
        return 2;
    }

    std::string error;
    const auto archive = genome::PakArchive::open(argv[1], &error);
    genome::Actor actor;
    genome::Motion motion;
    if (!archive || !genome::loadActor(archive->read(argv[2], &error), actor, &error) ||
        !genome::loadMotion(archive->read(argv[3], &error), motion, &error))
    {
        std::cerr << "error: " << error << "\n";
        return 1;
    }

    const genome::Skeleton skeleton = genome::buildSkeleton(actor);
    const std::vector<genome::Matrix4> pose = genome::samplePose(skeleton, motion, std::strtof(argv[4], nullptr));

    for (std::size_t index = 0; index < skeleton.bones.size(); ++index)
        std::printf("%s\t%.4f\t%.4f\t%.4f\n", skeleton.bones[index].name.c_str(), pose[index][12], pose[index][13],
                    pose[index][14]);
    return 0;
}
