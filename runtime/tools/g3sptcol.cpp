// What collision a SpeedTree definition declares, over the whole archive.
//
// SpeedTree's own SDK lets an artist put collision primitives on a tree, and a
// .spt stores them in token group 18000: a marker, a type, three vectors and a
// name. Our reader already keeps every token in file order, so answering this
// needed no new parsing - only reading what was there.
//
// The answer is that Gothic 3 uses none of it. All 98 definitions carry exactly
// one such object and every one of them is empty: type 0, all three vectors
// zero. So a tree's collision, if the game has any, does not come from here.

#include "genome/pak.h"
#include "genome/spt.h"

#include <cstdio>
#include <map>
#include <string>

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::puts("usage: g3sptcol <Speedtrees.pak> [name]   - the collision a definition declares");
        return 2;
    }

    std::string error;
    const auto archive = genome::PakArchive::open(argv[1], &error);
    if (!archive)
    {
        std::printf("error: %s\n", error.c_str());
        return 1;
    }

    std::size_t withCollision = 0, without = 0, objects = 0;
    std::map<int, std::size_t> byType;
    std::string firstWithout;

    for (const auto &entry : archive->entries())
    {
        genome::SpeedTree tree;
        if (!genome::loadSpeedTree(archive->read(entry.path, &error), tree, &error))
        {
            std::printf("%-56s will not read: %s\n", entry.path.c_str(), error.c_str());
            continue;
        }

        const bool wanted = argc >= 3 && entry.path.find(argv[2]) != std::string::npos;
        std::size_t here = 0;
        for (std::size_t at = 0; at < tree.tokens.size(); ++at)
        {
            if (tree.tokens[at].id != 18000)
                continue;
            ++here;

            // Everything up to the next 18000 or the end of the group belongs to
            // this object.
            int type = -1;
            std::vector<std::array<float, 3>> vectors;
            std::string name;
            for (std::size_t which = at + 1; which < tree.tokens.size(); ++which)
            {
                const genome::SpeedTreeToken &token = tree.tokens[which];
                if (token.id < 18001 || token.id > 18005)
                    break;
                if (token.id == 18001 && !token.floats.empty())
                    type = int(token.floats[0]);
                if (token.id >= 18002 && token.id <= 18004 && token.floats.size() >= 3)
                    vectors.push_back({token.floats[0], token.floats[1], token.floats[2]});
                if (token.id == 18005)
                    name = token.text;
            }
            ++byType[type];
            if (wanted)
            {
                std::printf("  object %zu: type %d, name \"%s\"\n", here, type, name.c_str());
                for (std::size_t v = 0; v < vectors.size(); ++v)
                    std::printf("    vector %zu: %8.3f %8.3f %8.3f\n", v, vectors[v][0], vectors[v][1],
                                vectors[v][2]);
            }
        }

        objects += here;
        if (here != 0)
            ++withCollision;
        else
        {
            ++without;
            if (firstWithout.empty())
                firstWithout = entry.path;
        }
        if (wanted)
            std::printf("%s: %zu collision objects\n", entry.path.c_str(), here);
    }

    std::printf("%zu trees carry collision (%zu objects in all), %zu carry none (e.g. %s)\n", withCollision,
                objects, without, firstWithout.c_str());
    for (const auto &[type, count] : byType)
        std::printf("  type %d: %zu\n", type, count);
    return 0;
}
