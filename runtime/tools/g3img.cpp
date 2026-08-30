// Loads textures out of an archive. With a name it dumps one image's header and
// optionally decodes a level to a .tga; without, it parses every image in the
// archive, which is the real test of the format work.

#include "genome/image.h"
#include "genome/pak.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>

namespace
{

// Uncompressed 32-bit TGA, BGRA with the origin top-left: the least code that
// gets a decoded level out of the runtime and in front of an eye.
bool writeTga(const std::string &path, std::uint32_t width, std::uint32_t height, const std::vector<std::uint8_t> &rgba)
{
    std::uint8_t header[18] = {};
    header[2] = 2; // uncompressed true colour
    header[12] = static_cast<std::uint8_t>(width & 0xFF);
    header[13] = static_cast<std::uint8_t>((width >> 8) & 0xFF);
    header[14] = static_cast<std::uint8_t>(height & 0xFF);
    header[15] = static_cast<std::uint8_t>((height >> 8) & 0xFF);
    header[16] = 32;   // bits per pixel
    header[17] = 0x28; // 8 alpha bits, rows top to bottom

    std::ofstream file(path, std::ios::binary);
    if (!file)
        return false;
    file.write(reinterpret_cast<const char *>(header), sizeof(header));

    std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * 4);
    for (std::uint32_t y = 0; y < height; ++y)
    {
        const std::uint8_t *source = rgba.data() + static_cast<std::size_t>(y) * width * 4;
        for (std::uint32_t x = 0; x < width; ++x)
        {
            row[x * 4 + 0] = source[x * 4 + 2];
            row[x * 4 + 1] = source[x * 4 + 1];
            row[x * 4 + 2] = source[x * 4 + 0];
            row[x * 4 + 3] = source[x * 4 + 3];
        }
        file.write(reinterpret_cast<const char *>(row.data()), static_cast<std::streamsize>(row.size()));
    }
    return static_cast<bool>(file);
}

// A decode that produced one flat colour is a decode that produced nothing, so
// the ranges are printed alongside the means.
void reportPixels(const std::vector<std::uint8_t> &rgba)
{
    std::uint8_t low[4] = {255, 255, 255, 255}, high[4] = {0, 0, 0, 0};
    std::uint64_t sum[4] = {};
    const std::size_t pixels = rgba.size() / 4;
    for (std::size_t pixel = 0; pixel < pixels; ++pixel)
    {
        for (int channel = 0; channel < 4; ++channel)
        {
            const std::uint8_t value = rgba[pixel * 4 + channel];
            low[channel] = std::min(low[channel], value);
            high[channel] = std::max(high[channel], value);
            sum[channel] += value;
        }
    }
    if (pixels == 0)
        return;

    static const char *names[4] = {"red", "green", "blue", "alpha"};
    for (int channel = 0; channel < 4; ++channel)
    {
        std::printf("  %-5s mean %5.1f   range %3u..%3u\n", names[channel],
                    static_cast<double>(sum[channel]) / static_cast<double>(pixels), low[channel], high[channel]);
    }
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::puts("usage: g3img <archive.pak> [image name] [decoded.tga [mip]]");
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
        genome::Image image;
        const std::vector<std::uint8_t> bytes = archive->read(argv[2], &error);
        if (bytes.empty() || !genome::loadImage(bytes, image, &error))
        {
            std::cerr << "error: " << error << "\n";
            return 1;
        }

        std::printf("%s: %ux%u %s, %u mip levels%s\n", argv[2], image.width, image.height,
                    genome::formatName(image.format), image.mipCount(), image.faceCount == 6 ? ", cube map" : "");
        std::printf("%zu bytes of pixel data, source asset was %u bytes\n", image.data.size(), image.sourceSize);
        for (std::uint32_t mip = 0; mip < image.mipCount(); ++mip)
        {
            const genome::ImageLevel level = image.level(mip);
            std::printf("  [%2u] %5ux%-5u %8zu bytes at %8zu\n", mip, level.width, level.height, level.size,
                        level.offset);
        }

        if (argc >= 4)
        {
            const std::uint32_t mip = argc > 4 ? static_cast<std::uint32_t>(std::atoi(argv[4])) : 0;
            std::vector<std::uint8_t> rgba;
            if (!genome::decodeLevel(image, mip, 0, rgba, &error))
            {
                std::cerr << "decode: " << error << "\n";
                return 1;
            }
            const genome::ImageLevel level = image.level(mip);
            if (!writeTga(argv[3], level.width, level.height, rgba))
            {
                std::cerr << "decode: could not write " << argv[3] << "\n";
                return 1;
            }
            std::printf("decoded mip %u (%ux%u) to %s\n", mip, level.width, level.height, argv[3]);
            reportPixels(rgba);
        }
        return 0;
    }

    std::size_t parsed = 0, failed = 0, pixels = 0, blockBytes = 0, truncated = 0, cubeMaps = 0;
    std::map<std::string, std::size_t> formats;
    std::map<std::string, std::size_t> reasons;
    for (const genome::PakEntry &entry : archive->entries())
    {
        if (entry.deleted || entry.path.find(".ximg") == std::string::npos)
            continue;

        genome::Image image;
        const std::vector<std::uint8_t> bytes = archive->read(entry, &error);
        if (bytes.empty() || !genome::loadImage(bytes, image, &error))
        {
            ++failed;
            if (reasons.size() < 20)
                ++reasons[error];
            continue;
        }
        ++parsed;
        pixels += static_cast<std::size_t>(image.width) * image.height;
        blockBytes += image.data.size();
        cubeMaps += image.faceCount == 6 ? 1 : 0;
        ++formats[genome::formatName(image.format)];

        // A full chain runs to 1x1; anything shorter is why the stored count has
        // to be honoured rather than derived.
        std::uint32_t full = 1;
        for (std::uint32_t side = std::max(image.width, image.height); side > 1; side /= 2)
            ++full;
        truncated += image.mipCount() < full ? 1 : 0;
    }

    std::printf("parsed %zu images, %zu failed\n", parsed, failed);
    std::printf("%zu texels at full resolution, %zu bytes of pixel data\n", pixels, blockBytes);
    std::printf("%zu with a truncated mip chain, %zu cube maps\n", truncated, cubeMaps);
    for (const auto &[format, count] : formats)
        std::printf("  %5zu  %s\n", count, format.c_str());
    for (const auto &[reason, count] : reasons)
        std::printf("  %5zu  %s\n", count, reason.c_str());
    return failed == 0 ? 0 : 1;
}
