// Lists what a sector places: entity names, world positions and the meshes they
// reference. With no name it parses every static sector in the archive, which
// is how the format work is checked.

#include "genome/pak.h"
#include "genome/world.h"

#include <cstdio>
#include <iostream>
#include <map>

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::puts("usage: g3sector <Projects_compiled.pak> [sector.node]");
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
        genome::WorldLayer layer;
        if (!genome::loadWorldNode(archive->read(argv[2], &error), layer, &error))
        {
            std::cerr << "error: " << error << "\n";
            return 1;
        }

        std::printf("%zu entities, %zu with a static mesh\n", layer.placements.size(), layer.meshCount());
        std::size_t shown = 0;
        for (const genome::Placement &placement : layer.placements)
        {
            if (placement.meshName.empty() || shown++ >= 20)
                continue;
            const std::array<float, 3> at = placement.translation();
            std::printf("  %-34s %9.0f %8.0f %9.0f  %s\n", placement.name.c_str(), at[0], at[1], at[2],
                        placement.meshName.c_str());
        }
        return 0;
    }

    std::size_t parsed = 0, failed = 0, entities = 0, meshes = 0, vegetation = 0, plantMeshes = 0;
    std::map<std::string, std::size_t> reasons;
    for (const genome::PakEntry &entry : archive->entries())
    {
        if (entry.deleted || entry.path.find("_cstat.node") == std::string::npos)
            continue;

        genome::WorldLayer layer;
        if (!genome::loadWorldNode(archive->read(entry, &error), layer, &error))
        {
            ++failed;
            if (reasons.size() < 12)
                ++reasons[error];
            continue;
        }
        ++parsed;
        entities += layer.placements.size();
        vegetation += layer.vegetation.size();
        plantMeshes += layer.vegetationMeshes.size();
        meshes += layer.meshCount();
    }

    std::printf("parsed %zu static sectors, %zu failed\n", parsed, failed);
    std::printf("%zu entities, %zu placing a static mesh, %zu plants from %zu plant meshes\n", entities, meshes,
                vegetation, plantMeshes);
    for (const auto &[reason, count] : reasons)
        std::printf("  %5zu  %s\n", count, reason.c_str());
    return failed == 0 ? 0 : 1;
}
