// Loads actors: skeleton, skinned mesh and weights. With no name it parses the
// whole archive, which is how we know the format work holds up.

#include "genome/actor.h"
#include "genome/pak.h"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <map>

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::puts("usage: g3actor <archive.pak> [actor name]");
        return 2;
    }

    std::string error;
    const auto archive = genome::PakArchive::open(argv[1], &error);
    if (!archive)
    {
        std::cerr << "error: " << error << "\n";
        return 1;
    }

    if (argc >= 3)
    {
        genome::Actor actor;
        const std::vector<std::uint8_t> bytes = archive->read(argv[2], &error);
        if (bytes.empty() || !genome::loadActor(bytes, actor, &error))
        {
            std::cerr << "error: " << error << "\n";
            return 1;
        }

        std::array<float, 3> min{}, max{};
        actor.computeBounds(min, max);
        std::printf("%zu nodes, %zu submeshes, %zu vertices, %zu triangles\n", actor.nodes.size(),
                    actor.submeshes.size(), actor.vertexCount(), actor.triangleCount());
        std::printf("bind pose spans %.1f x %.1f x %.1f cm\n", max[0] - min[0], max[1] - min[1], max[2] - min[2]);

        std::size_t roots = 0, maxInfluences = 0;
        for (const genome::Node &node : actor.nodes)
            roots += node.parent < 0 ? 1 : 0;
        for (const auto &list : actor.influences)
            maxInfluences = std::max(maxInfluences, list.size());
        std::printf("%zu roots, %zu skinned vertices, up to %zu influences each\n", roots, actor.influences.size(),
                    maxInfluences);

        std::puts("materials:");
        for (std::size_t index = 0; index < actor.materials.size(); ++index)
            if (!actor.materials[index].empty())
                std::printf("  [%zu] %s\n", index, actor.materials[index].c_str());

        std::puts("skeleton (first levels):");
        for (std::size_t index = 0; index < actor.nodes.size(); ++index)
        {
            const genome::Node &node = actor.nodes[index];
            int depth = 0;
            for (int parent = node.parent; parent >= 0; parent = actor.nodes[parent].parent)
                ++depth;
            if (depth > 2)
                continue;
            std::printf("  %*s%-28s  y=%7.1f\n", depth * 2, "", node.name.c_str(), node.globalBind[13]);
        }
        return 0;
    }

    std::size_t parsed = 0, failed = 0, withMesh = 0, nodes = 0, vertices = 0;
    std::map<std::string, std::size_t> reasons;
    for (const genome::PakEntry &entry : archive->entries())
    {
        if (entry.deleted || entry.path.find(".xact") == std::string::npos)
            continue;

        genome::Actor actor;
        const std::vector<std::uint8_t> bytes = archive->read(entry, &error);
        if (bytes.empty() || !genome::loadActor(bytes, actor, &error))
        {
            ++failed;
            if (reasons.size() < 20)
                ++reasons[error];
            continue;
        }
        ++parsed;
        nodes += actor.nodes.size();
        vertices += actor.vertexCount();
        if (!actor.submeshes.empty())
            ++withMesh;
    }

    std::printf("parsed %zu actors, %zu failed\n", parsed, failed);
    std::printf("%zu with a mesh, %zu bones, %zu vertices in total\n", withMesh, nodes, vertices);
    for (const auto &[reason, count] : reasons)
        std::printf("  %5zu  %s\n", count, reason.c_str());
    return failed == 0 ? 0 : 1;
}
