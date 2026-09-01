// Lists what a sector places: entity names, world positions and the meshes they
// reference. With no name it parses every static sector in the archive, which
// is how the format work is checked.

#include "genome/pak.h"
#include "genome/world.h"

#include <cstdio>
#include <iostream>
#include <algorithm>
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

        std::printf("%zu lights in this sector\n", layer.lights.size());
        std::printf("%zu entities, %zu with a static mesh\n", layer.placements.size(), layer.meshCount());
        std::size_t shown = 0;
        for (const genome::Placement &placement : layer.placements)
        {
            if (placement.meshName.empty() || shown++ >= 20)
                continue;
            const std::array<float, 3> at = placement.translation();
            std::printf("  %-34s %9.0f %8.0f %9.0f  {%s}  %s\n", placement.name.c_str(), at[0], at[1], at[2],
                        placement.guid.c_str(), placement.meshName.c_str());
        }
        return 0;
    }

    std::size_t parsed = 0, failed = 0, entities = 0, meshes = 0, vegetation = 0, plantMeshes = 0, trees = 0;
    std::map<std::string, std::size_t> reasons;
    // Per definition: how many stand in the world and how tall they grew, which
    // is the yardstick a generator has to hit.
    struct TreeStats
    {
        std::size_t count = 0;
        float lowest = 1e9f, tallest = 0.0f, total = 0.0f;
    };
    std::map<std::string, TreeStats> byResource;
    std::size_t lights = 0, shadowing = 0;
    float shortestRange = 1e9f, longestRange = 0.0f;
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
        trees += layer.trees.size();
        lights += layer.lights.size();
        for (const genome::PointLight &light : layer.lights)
        {
            shortestRange = std::min(shortestRange, light.range);
            longestRange = std::max(longestRange, light.range);
            shadowing += light.castShadows ? 1 : 0;
        }
        for (const genome::TreePlacement &tree : layer.trees)
        {
            TreeStats &stats = byResource[tree.resource];
            const float height = tree.boundsHeight();
            ++stats.count;
            stats.total += height;
            stats.lowest = std::min(stats.lowest, height);
            stats.tallest = std::max(stats.tallest, height);
        }
        plantMeshes += layer.vegetationMeshes.size();
        meshes += layer.meshCount();
    }

    std::printf("parsed %zu static sectors, %zu failed\n", parsed, failed);
    std::printf("%zu entities, %zu placing a static mesh, %zu plants from %zu plant meshes\n", entities, meshes,
                vegetation, plantMeshes);
    std::printf("%zu trees from %zu definitions\n", trees, byResource.size());
    if (lights != 0)
        std::printf("%zu static point lights, %zu casting shadows, range %.0f to %.0f units\n", lights, shadowing,
                    shortestRange, longestRange);
    for (const auto &[reason, count] : reasons)
        std::printf("  %5zu  %s\n", count, reason.c_str());

    for (const auto &[resource, stats] : byResource)
        std::printf("  %-46s %6zu  height %6.0f .. %-6.0f mean %6.0f\n", resource.c_str(), stats.count, stats.lowest,
                    stats.tallest, stats.total / float(stats.count));
    return failed == 0 ? 0 : 1;
}
