// What a SpeedTree definition declares, over the whole archive.
//
// Written to answer whether Gothic 3's trees carry their own collision. The
// first answer this tool gave was no, and it was wrong: it read token group
// 18000 as SpeedTree's SCollisionObject because the fields have that shape, and
// printed a "type" that came from an initialiser rather than from any byte.
// Group 18000 is the baked shadow - three orthonormal vectors and
// CompositeShadowMap.tga - which our own .spt reader already knew, having named
// id 18005 shadowTexture.
//
// The collision is at ids 12002, 12003 and 12004: a sphere, a cylinder and a
// box, in metres. Every one of the 98 definitions carries at least one.
//
// So this prints the ids it actually saw, and --ids gives a census of every id
// in the archive rather than only the group someone already suspected.

#include "genome/pak.h"
#include "genome/spt.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

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

    std::size_t files = 0, groups = 0, empty = 0;
    std::map<std::uint32_t, std::size_t> idCounts; // which ids the group actually uses

    for (const auto &entry : archive->entries())
    {
        genome::SpeedTree tree;
        if (!genome::loadSpeedTree(archive->read(entry.path, &error), tree, &error))
        {
            std::printf("%-56s will not read: %s\n", entry.path.c_str(), error.c_str());
            continue;
        }
        ++files;
        const bool wanted = argc >= 3 && entry.path.find(argv[2]) != std::string::npos;

        for (std::size_t at = 0; at < tree.tokens.size(); ++at)
        {
            if (tree.tokens[at].id != 18000)
                continue;
            ++groups;

            // Everything up to the first id outside the group belongs to it.
            std::vector<std::pair<std::uint32_t, std::vector<float>>> values;
            std::string name;
            bool anythingSet = false;
            for (std::size_t which = at + 1; which < tree.tokens.size(); ++which)
            {
                const genome::SpeedTreeToken &token = tree.tokens[which];
                if (token.id < 18001 || token.id > 18005)
                    break;
                ++idCounts[token.id];
                if (token.id == 18005)
                    name = token.text;
                else
                    values.push_back({token.id, token.floats});
                for (float value : token.floats)
                    if (std::fabs(value) > 1e-12f)
                        anythingSet = true;
                if (!token.text.empty())
                    anythingSet = true;
            }

            if (!anythingSet)
                ++empty;
            // A definition that sets anything at all is worth seeing whether or
            // not it was asked for: it is the whole question.
            if (wanted || anythingSet)
            {
                std::printf("%s\n", entry.path.c_str());
                for (const auto &[id, floats] : values)
                {
                    std::printf("  %u:", id);
                    for (float value : floats)
                        std::printf(" %10.4f", value);
                    if (floats.empty())
                        std::printf(" (no payload)");
                    std::printf("\n");
                }
                if (!name.empty())
                    std::printf("  18005: \"%s\"\n", name.c_str());
            }
        }
    }

    // A census of every id, so no group has to be guessed at from its widths
    // again. For each: how many files carry it, how many carry a non-zero value,
    // and the range of what it holds.
    // Every token of one definition, in file order.
    // One line per definition: the size it declares, then the 12002/12003/12004
    // groups, so the numbers can be read against something known.
    if (argc >= 3 && std::string(argv[2]) == "--table")
    {
        std::printf("%-52s %8s  %s\n", "definition", "size", "12002 / 12003 / 12004");
        for (const auto &entry : archive->entries())
        {
            genome::SpeedTree tree;
            if (!genome::loadSpeedTree(archive->read(entry.path, &error), tree, &error))
                continue;
            float size = 0.0f;
            std::string groups;
            char line[256];
            for (const genome::SpeedTreeToken &token : tree.tokens)
            {
                if (token.id == 2006 && !token.floats.empty())
                    size = token.floats[0];
                if (token.id < 12002 || token.id > 12004 || token.floats.empty())
                    continue;
                std::snprintf(line, sizeof(line), "%u[", token.id);
                groups += line;
                for (std::size_t at = 0; at < token.floats.size(); ++at)
                {
                    std::snprintf(line, sizeof(line), "%s%.3f", at ? " " : "", token.floats[at]);
                    groups += line;
                }
                groups += "] ";
            }
            std::printf("%-52s %8.2f  %s\n", entry.path.c_str(), size, groups.c_str());
        }
        return 0;
    }

    if (argc >= 4 && std::string(argv[2]) == "--dump")
    {
        for (const auto &entry : archive->entries())
        {
            if (entry.path.find(argv[3]) == std::string::npos)
                continue;
            genome::SpeedTree tree;
            if (!genome::loadSpeedTree(archive->read(entry.path, &error), tree, &error))
                continue;
            std::printf("%s\n", entry.path.c_str());
            for (const genome::SpeedTreeToken &token : tree.tokens)
            {
                if (token.floats.empty() && token.text.empty())
                    continue;
                std::printf("  %-6u", token.id);
                for (float value : token.floats)
                    std::printf(" %10.4f", value);
                if (!token.text.empty())
                    std::printf(" \"%s\"", token.text.c_str());
                std::printf("\n");
            }
            return 0;
        }
        std::printf("no definition matching %s\n", argv[3]);
        return 1;
    }

    if (argc >= 3 && std::string(argv[2]) == "--ids")
    {
        struct Census
        {
            std::size_t files = 0, nonZero = 0, floats = 0, strings = 0;
            float low = 0.0f, high = 0.0f;
            bool any = false;
            std::string sample;
        };
        std::map<std::uint32_t, Census> census;
        for (const auto &entry : archive->entries())
        {
            genome::SpeedTree tree;
            if (!genome::loadSpeedTree(archive->read(entry.path, &error), tree, &error))
                continue;
            std::set<std::uint32_t> here, nonZeroHere;
            for (const genome::SpeedTreeToken &token : tree.tokens)
            {
                here.insert(token.id);
                Census &c = census[token.id];
                c.floats += token.floats.size();
                if (!token.text.empty())
                {
                    ++c.strings;
                    if (c.sample.empty())
                        c.sample = token.text;
                }
                for (float value : token.floats)
                {
                    if (std::fabs(value) > 1e-12f)
                        nonZeroHere.insert(token.id);
                    if (!c.any)
                    {
                        c.low = c.high = value;
                        c.any = true;
                    }
                    c.low = value < c.low ? value : c.low;
                    c.high = value > c.high ? value : c.high;
                }
            }
            for (std::uint32_t id : here)
                ++census[id].files;
            for (std::uint32_t id : nonZeroHere)
                ++census[id].nonZero;
        }
        std::printf("%-8s %6s %8s %8s %8s  %12s %12s  %s\n", "id", "files", "non-zero", "floats", "strings",
                    "low", "high", "sample text");
        for (const auto &[id, c] : census)
            std::printf("%-8u %6zu %8zu %8zu %8zu  %12.4f %12.4f  %s\n", id, c.files, c.nonZero, c.floats,
                        c.strings, c.low, c.high, c.sample.c_str());
        return 0;
    }

    std::printf("%zu definitions, %zu collision groups, %zu of them entirely zero\n", files, groups, empty);
    std::printf("ids the groups use:");
    for (const auto &[id, count] : idCounts)
        std::printf(" %u x%zu", id, count);
    std::printf("\n");
    return 0;
}
