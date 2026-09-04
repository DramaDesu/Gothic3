// What a .lrentdat holds: the people, the things and the markers a world places.
//
// The sectors carry what the world is built from. This carries what is in it -
// NPCs with their inventories, dialogue and daily routines, items on the ground,
// and the waypoints those routines refer to.

#include "genome/pak.h"
#include "genome/world.h"

#include <cstdio>
#include <map>
#include <string>

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::puts("usage: g3ent <Projects_compiled.pak> [name]   - entities, or one file's in full");
        return 2;
    }

    std::string error;
    const auto archive = genome::PakArchive::open(argv[1], &error);
    if (!archive)
    {
        std::printf("error: %s\n", error.c_str());
        return 1;
    }

    std::size_t files = 0, entities = 0, drawn = 0;
    std::map<std::string, std::size_t> byClass;

    for (const genome::PakEntry &entry : archive->entries())
    {
        if (entry.deleted || entry.path.size() < 10 ||
            entry.path.compare(entry.path.size() - 9, 9, ".lrentdat") != 0)
            continue;
        const bool wanted = argc >= 3 && entry.path.find(argv[2]) != std::string::npos;
        if (argc >= 3 && !wanted)
            continue;

        std::vector<genome::DynamicEntity> found;
        if (!genome::loadEntityFile(archive->read(entry.path, &error), found, &error))
        {
            std::printf("%-64s %s\n", entry.path.c_str(), error.c_str());
            continue;
        }

        ++files;
        entities += found.size();
        for (const genome::DynamicEntity &one : found)
        {
            for (const std::string &className : one.classes)
                ++byClass[className];
            if (!one.actorName.empty())
                ++drawn;
            if (wanted)
            {
                const std::array<float, 3> at = one.translation();
                std::printf("  %-34s %8.0f %8.0f %8.0f  %-34s %s\n", one.name.c_str(), double(at[0]),
                            double(at[1]), double(at[2]), one.actorName.c_str(),
                            one.has("gCNPC_PS") ? "npc" : "");
            }
        }
        if (wanted)
            std::printf("%s: %zu entities\n", entry.path.c_str(), found.size());
    }

    std::printf("%zu files, %zu entities, %zu of them wearing an actor\n", files, entities, drawn);
    std::printf("the property sets they carry, most common first:\n");
    std::vector<std::pair<std::size_t, std::string>> order;
    for (const auto &entry : byClass)
        order.push_back({entry.second, entry.first});
    std::sort(order.rbegin(), order.rend());
    for (std::size_t index = 0; index < order.size() && index < 20; ++index)
        std::printf("  %-34s %zu\n", order[index].second.c_str(), order[index].first);
    return 0;
}
