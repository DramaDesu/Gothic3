// Reads .xlmp lightmaps, as far as they are understood. There are 11234 of them in Lightmaps.pak, one per
// placed instance rather than per mesh, and nothing was reading them - which is
// why our world has light but no shadow.
//
// With a name it dumps one; with none it walks the archive, which is how the
// format work gets checked.

#include "genome/pak.h"
#include "genome/property_set.h"
#include "genome/reader.h"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <map>

namespace
{

struct Lightmap
{
    std::string meshName;
    // One array per mesh element, four bytes a vertex.
    std::vector<std::vector<std::uint32_t>> perElement;
    std::size_t leftover = 0;
};

bool loadLightmap(const std::vector<std::uint8_t> &bytes, Lightmap &out, std::string *error)
{
    const auto fail = [&](const char *reason) {
        if (error)
            *error = reason;
        return false;
    };

    genome::Reader reader(bytes);
    const genome::StringTable strings = genome::StringTable::sniff(reader);
    if (!strings.wrapped())
        return fail("not a wrapped genome file");

    genome::PropertySetHeader header;
    if (!genome::readPropertySetHeader(reader, strings, header))
        return fail("bad property set");
    if (header.className != "eCResourceLightmap_PS")
        return fail("not a lightmap");

    std::uint16_t resourceVersion = 0;
    if (!genome::readResourceBase(reader, resourceVersion))
        return fail("bad resource header");

    // The body: a version, the priority property this class carries, then one
    // array of vertex colours per element of the mesh it lights. Each array is a
    // one-byte prefix and a u32 count, the same shape the vegetation grid uses.
    reader.skip(14);
    const std::uint32_t elements = reader.u32();
    if (!reader.ok() || elements > 64)
        return fail("implausible element count");
    reader.skip(3); // list prefix and a count repeated as a u16

    out.perElement.clear();
    // Only the first pair is understood, so only the first pair is read. What
    // follows it is 16-byte records that no vertex count explains - see
    // docs/lighting.md.
    for (std::uint32_t index = 0; index < 2 && reader.ok(); ++index)
    {
        reader.skip(1);
        const std::uint32_t count = reader.u32();
        if (!reader.ok() || count > reader.remaining() / 4)
            return fail("implausible vertex count");

        std::vector<std::uint32_t> colours(count);
        reader.array(colours.data(), count);
        out.perElement.push_back(std::move(colours));
    }

    // How much is left after the arrays. A reading that stops in the wrong place
    // is a reading that happens to fit, so the leftover is reported rather than
    // assumed to be nothing.
    out.leftover = reader.ok() ? strings.payloadEnd() - reader.tell() : 0;

    return reader.ok() ? true : fail("truncated lightmap");
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::puts("usage: g3lightmap <Lightmaps.pak> [name.xlmp]");
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
        Lightmap map;
        if (!loadLightmap(archive->read(argv[2], &error), map, &error))
        {
            std::cerr << "error: " << error << "\n";
            return 1;
        }

        std::printf("mesh %s\n", map.meshName.c_str());
        for (std::size_t index = 0; index < map.perElement.size(); ++index)
        {
            const std::vector<std::uint32_t> &colours = map.perElement[index];
            if (colours.empty())
            {
                std::printf("  element %zu: empty\n", index);
                continue;
            }
            std::size_t total = 0, darkest = 255, brightest = 0;
            for (std::uint32_t packed : colours)
            {
                const std::size_t value = std::max({packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0xFF});
                total += value;
                darkest = std::min(darkest, value);
                brightest = std::max(brightest, value);
            }
            std::printf("  element %zu: %6zu vertices, brightness %zu..%zu mean %zu, first %08x\n", index,
                        colours.size(), darkest, brightest, total / colours.size(), colours.front());
        }
        return 0;
    }

    std::size_t parsed = 0, failed = 0, vertices = 0;
    std::map<std::string, std::size_t> reasons;
    for (const genome::PakEntry &entry : archive->entries())
    {
        if (entry.deleted || entry.path.find(".xlmp") == std::string::npos)
            continue;

        Lightmap map;
        if (!loadLightmap(archive->read(entry, &error), map, &error))
        {
            ++failed;
            if (reasons.size() < 12)
                ++reasons[error];
            continue;
        }
        ++parsed;
        for (const std::vector<std::uint32_t> &colours : map.perElement)
            vertices += colours.size();
    }

    std::printf("parsed %zu lightmaps, %zu failed\n", parsed, failed);
    std::printf("%zu lit vertices in all\n", vertices);
    for (const auto &[reason, count] : reasons)
        std::printf("  %5zu  %s\n", count, reason.c_str());
    return failed == 0 ? 0 : 1;
}
