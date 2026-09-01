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
    float scale = 0.0f;          // a per-instance scalar, 1.0, 0.84 and 0.28 in three of one mesh
    std::uint32_t elements = 0;  // matches the element count of the mesh it names

    // Baked lighting for the first element: a packed colour and a unit direction
    // per vertex.
    std::vector<std::uint32_t> colours;
    std::vector<float> directions;
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

    // The body: a per-instance scalar, the priority this class carries, the
    // element count of the mesh, then the baked lighting itself - a colour and a
    // direction for every vertex of the first element, each as a prefixed array.
    reader.skip(4);
    out.scale = reader.f32();
    reader.skip(6);
    const std::uint32_t elements = reader.u32();
    if (!reader.ok() || elements > 64)
        return fail("implausible element count");
    out.elements = elements;
    reader.skip(3);

    reader.skip(1);
    const std::uint32_t count = reader.u32();
    if (!reader.ok() || count > reader.remaining() / 16)
        return fail("implausible vertex count");

    out.colours.resize(count);
    reader.array(out.colours.data(), count);

    // The same count again, of unit vectors - the direction the light came from,
    // which is what a normal-mapped surface needs and a flat colour cannot give.
    reader.skip(1);
    if (reader.u32() != count)
        return fail("the direction array does not match the colours");
    out.directions.resize(std::size_t(count) * 3);
    reader.array(out.directions.data(), out.directions.size());

    // The record ends with a marker, as the sector entities do.
    if (reader.u16() != 0x03d5 && !reader.ok())
        return fail("truncated before the marker");
    if (reader.u32() != 0x11223344)
        return fail("no end marker after the vertex data");

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

        std::printf("scale %.3f, %u elements, %zu lit vertices, %zu bytes not read\n", map.scale, map.elements,
                    map.colours.size(), map.leftover);

        std::size_t total = 0, darkest = 255, brightest = 0;
        for (std::uint32_t packed : map.colours)
        {
            const std::size_t value = std::max({packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0xFF});
            total += value;
            darkest = std::min(darkest, value);
            brightest = std::max(brightest, value);
        }
        if (!map.colours.empty())
            std::printf("brightness %zu..%zu mean %zu, first colour %08x, first direction %.3f %.3f %.3f\n",
                        darkest, brightest, total / map.colours.size(), map.colours.front(), map.directions[0],
                        map.directions[1], map.directions[2]);
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
        vertices += map.colours.size();
    }

    std::printf("parsed %zu lightmaps, %zu failed\n", parsed, failed);
    std::printf("%zu lit vertices in all\n", vertices);
    for (const auto &[reason, count] : reasons)
        std::printf("  %5zu  %s\n", count, reason.c_str());
    return failed == 0 ? 0 : 1;
}
