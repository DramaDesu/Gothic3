// Reads baked lighting. There are 11234 .xlmp files in Lightmaps.pak, one per
// placed instance, and nothing was reading them - which is why our world has
// light but no shadow.
//
// With a name it dumps one; with none it walks the archive, which is how the
// format work gets checked.

#include "genome/lightmap.h"
#include "genome/pak.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>

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

    // With an index as well, the bitmap is written out so it can be looked at.
    const int wanted = argc >= 4 ? std::atoi(argv[3]) : -1;

    if (argc >= 3)
    {
        genome::Lightmap map;
        if (!genome::loadLightmap(archive->read(argv[2], &error), map, &error))
        {
            std::cerr << "error: " << error << "\n";
            return 1;
        }

        std::printf("mesh %s\n", map.meshName.c_str());
        std::printf("%s, scaling %.3f, priority %.3f, %zu elements\n",
                    map.type == genome::LightmapType::Mixed ? "mixed" : "per vertex", map.scaling,
                    map.resourcePriority, map.elements.size());

        for (std::size_t index = 0; index < map.elements.size(); ++index)
        {
            const genome::LightmapElement &element = map.elements[index];
            if (element.colours.empty())
            {
                std::printf("  element %zu: no lighting\n", index);
                continue;
            }

            // The top byte is how much sky the vertex can see; the rest is the
            // light that reached it.
            std::size_t light = 0, occlusion = 0;
            for (std::uint32_t packed : element.colours)
            {
                light += std::max({packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0xFF});
                occlusion += (packed >> 24) & 0xFF;
            }
            const std::size_t vertices = element.colours.size();
            std::printf("  element %zu: %6zu vertices, light %3zu, sky %3zu, %zu bitmaps\n", index, vertices,
                        light / vertices, occlusion / vertices, element.bitmaps.size());
            for (std::size_t bitmap = 0; bitmap < element.bitmaps.size() && bitmap < 3; ++bitmap)
            {
                const genome::LightmapBitmap &image = element.bitmaps[bitmap];
                std::printf("    bitmap %zu: %d x %d at %d,%d in uv set %d, %zu bytes\n", bitmap, image.width,
                            image.height, image.offsetX, image.offsetY, image.uvSet, image.data.size());
            }
        }
        if (wanted >= 0)
        {
            std::size_t seen = 0;
            for (const genome::LightmapElement &element : map.elements)
                for (const genome::LightmapBitmap &bitmap : element.bitmaps)
                {
                    if (int(seen++) != wanted)
                        continue;
                    if (bitmap.data.size() != std::size_t(bitmap.width) * bitmap.height * 4)
                    {
                        std::printf("bitmap %d is %zu bytes for %d x %d, not four a texel\n", wanted,
                                    bitmap.data.size(), bitmap.width, bitmap.height);
                        return 1;
                    }

                    std::FILE *out = std::fopen("bitmap.ppm", "wb");
                    if (!out)
                        return 1;
                    std::fprintf(out, "P6\n%d %d\n255\n", bitmap.width, bitmap.height);
                    for (std::size_t at = 0; at < bitmap.data.size(); at += 4)
                    {
                        // Stored blue first, as everything in this engine is.
                        std::fputc(bitmap.data[at + 2], out);
                        std::fputc(bitmap.data[at + 1], out);
                        std::fputc(bitmap.data[at + 0], out);
                    }
                    std::fclose(out);
                    std::printf("wrote bitmap.ppm, %d x %d\n", bitmap.width, bitmap.height);
                    return 0;
                }
            std::printf("no bitmap %d here\n", wanted);
            return 1;
        }
        return 0;
    }

    std::size_t parsed = 0, failed = 0, vertices = 0, mixed = 0, bitmaps = 0;
    // What the two halves of the colour actually hold across the whole game.
    std::size_t totalLight = 0, totalSky = 0, litVertices = 0;
    // Whether every bitmap really is four bytes a texel.
    std::size_t sized = 0, oddlySized = 0;
    // How big a patch gets, which decides whether a fixed-tile atlas allocator
    // can hold every one of them.
    std::int32_t widest = 0, tallest = 0;
    std::size_t over64 = 0, over128 = 0, over256 = 0;
    std::map<std::string, std::size_t> reasons;
    for (const genome::PakEntry &entry : archive->entries())
    {
        if (entry.deleted || entry.path.find(".xlmp") == std::string::npos)
            continue;

        genome::Lightmap map;
        if (!genome::loadLightmap(archive->read(entry, &error), map, &error))
        {
            ++failed;
            if (reasons.size() < 12)
                ++reasons[error];
            continue;
        }
        ++parsed;
        vertices += map.vertexCount();
        for (const genome::LightmapElement &element : map.elements)
            for (std::uint32_t packed : element.colours)
            {
                totalLight += std::max({packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0xFF});
                totalSky += (packed >> 24) & 0xFF;
                litVertices += ((packed & 0xFFFFFF) != 0) ? 1 : 0;
            }
        mixed += map.type == genome::LightmapType::Mixed ? 1 : 0;
        for (const genome::LightmapElement &element : map.elements)
        {
            bitmaps += element.bitmaps.size();
            for (const genome::LightmapBitmap &bitmap : element.bitmaps)
            {
                if (bitmap.data.empty())
                    continue;
                widest = std::max(widest, bitmap.width);
                tallest = std::max(tallest, bitmap.height);
                over64 += (bitmap.width > 64 || bitmap.height > 64) ? 1 : 0;
                over128 += (bitmap.width > 128 || bitmap.height > 128) ? 1 : 0;
                over256 += (bitmap.width > 256 || bitmap.height > 256) ? 1 : 0;
                if (bitmap.data.size() == std::size_t(bitmap.width) * bitmap.height * 4)
                    ++sized;
                else
                    ++oddlySized;
            }
        }
    }

    std::printf("parsed %zu lightmaps, %zu failed\n", parsed, failed);
    std::printf("%zu lit vertices, %zu instances lit by bitmaps as well, %zu bitmaps in all\n", vertices, mixed,
                bitmaps);
    std::printf("%zu bitmaps are four bytes a texel, %zu are not\n", sized, oddlySized);
    std::printf("widest patch %d, tallest %d; %zu over 64, %zu over 128, %zu over 256\n", widest, tallest, over64,
                over128, over256);
    if (vertices != 0)
        std::printf("mean baked light %.1f of 255, mean sky %.1f of 255, %.1f%% of vertices carry any light\n",
                    double(totalLight) / double(vertices), double(totalSky) / double(vertices),
                    100.0 * double(litVertices) / double(vertices));
    for (const auto &[reason, count] : reasons)
        std::printf("  %5zu  %s\n", count, reason.c_str());
    return failed == 0 ? 0 : 1;
}
