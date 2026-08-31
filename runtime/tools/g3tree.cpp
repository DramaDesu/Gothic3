// Reads SpeedTree definitions. With a name it prints one tree's parameters;
// with none it parses every tree in the archive, which is how the width table is
// checked - a wrong width does not fail on its own, it desynchronises, so the
// count of files that parse to the last byte is the whole test.

#include "genome/pak.h"
#include "genome/spt.h"
#include "genome/tree.h"

#include <cstdio>
#include <iostream>
#include <algorithm>
#include <map>
#include <vector>

namespace
{

void printTree(const genome::SpeedTree &tree)
{
    std::printf("bark      %s\n", tree.barkTexture.c_str());
    std::printf("size      %.1f +/- %.1f\n", tree.size, tree.sizeVariance);
    std::printf("2001/2003 %.1f %.2f\n", tree.parameter2001, tree.parameter2003);
    std::printf("seed      %u\n", tree.seed);
    std::printf("billboard %s\n", tree.billboardTexture.c_str());
    std::printf("shadow    %s\n", tree.shadowTexture.c_str());
    std::printf("%zu branch levels, %zu leaf kinds, %zu materials, %zu tokens\n", tree.levels.size(),
                tree.leaves.size(), tree.materials.size(), tree.tokens.size());

    for (std::size_t index = 0; index < tree.leaves.size(); ++index)
    {
        const genome::LeafKind &leaf = tree.leaves[index];
        std::printf("  leaf %zu: %-32s size %.2f x %.2f  atlas tile %s\n", index, leaf.texture.c_str(),
                    leaf.size[0], leaf.size[1], leaf.hasCorners ? "yes" : "no");
    }

    for (std::size_t index = 0; index < tree.levels.size(); ++index)
    {
        const genome::BranchLevel &level = tree.levels[index];
        std::printf("  level %zu:", index);
        for (float number : level.numbers)
            std::printf(" %g", number);
        std::printf("\n");
        for (std::size_t slot = 0; slot < 9; ++slot)
        {
            const genome::Spline &spline = level.profiles[slot];
            if (spline.points.empty())
                continue;
            std::printf("    curve %zu  %8.3g .. %-8.3g var %-6.3g %zu points   "
                        "at 0/.5/1: %8.3g %8.3g %8.3g\n",
                        slot, spline.low, spline.high, spline.variance, spline.points.size(), spline.at(0.0f),
                        spline.at(0.5f), spline.at(1.0f));
        }
    }
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::puts("usage: g3tree <Speedtrees.pak> [tree.spt]");
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
        genome::SpeedTree tree;
        if (!genome::loadSpeedTree(archive->read(argv[2], &error), tree, &error))
        {
            std::cerr << "error: " << error << "\n";
            return 1;
        }
        printTree(tree);

        genome::Mesh grown;
        genome::TreeGrowth growth;
        if (!genome::growTree(tree, tree.seed, growth, grown))
        {
            std::puts("could not grow this definition");
            return 1;
        }
        std::printf("grown: %zu vertices, %zu triangles, %.0f units tall, %.0f wide\n", grown.vertexCount(),
                    grown.triangleCount(), grown.boundsMax[1] - grown.boundsMin[1],
                    grown.boundsMax[0] - grown.boundsMin[0]);

        // Where the geometry actually sits. A tree-shaped failure and a tree
        // look alike at a glance, and quartiles do not.
        for (const genome::MeshElement &element : grown.elements)
        {
            std::vector<float> heights;
            for (const std::array<float, 3> &position : element.positions)
                heights.push_back(position[1]);
            if (heights.empty())
                continue;
            std::sort(heights.begin(), heights.end());
            std::printf("  %-40s %6zu vertices, y %5.0f | %5.0f | %5.0f | %5.0f | %5.0f\n",
                        element.materialName.c_str(), heights.size(), heights.front(),
                        heights[heights.size() / 4], heights[heights.size() / 2], heights[heights.size() * 3 / 4],
                        heights.back());
        }
        return 0;
    }

    std::size_t parsed = 0, failed = 0, levels = 0, leaves = 0, grown_ok = 0, triangles = 0;
    float heights = 0.0f;
    std::map<std::string, std::size_t> reasons;
    for (const genome::PakEntry &entry : archive->entries())
    {
        if (entry.deleted || entry.path.find(".spt") == std::string::npos)
            continue;

        genome::SpeedTree tree;
        if (!genome::loadSpeedTree(archive->read(entry, &error), tree, &error))
        {
            ++failed;
            ++reasons[error];
            continue;
        }
        ++parsed;
        levels += tree.levels.size();
        leaves += tree.leaves.size();

        genome::Mesh grown;
        if (genome::growTree(tree, tree.seed, genome::TreeGrowth{}, grown))
        {
            ++grown_ok;
            triangles += grown.triangleCount();
            heights += grown.boundsMax[1] - grown.boundsMin[1];
        }
    }

    std::printf("parsed %zu trees, %zu failed\n", parsed, failed);
    std::printf("%zu branch levels, %zu leaf kinds in all\n", levels, leaves);
    if (grown_ok != 0)
        std::printf("grew %zu, %zu triangles in all, mean height %.0f units\n", grown_ok, triangles,
                    heights / float(grown_ok));
    for (const auto &[reason, count] : reasons)
        std::printf("  %4zu  %s\n", count, reason.c_str());
    return failed == 0 ? 0 : 1;
}
