#include "image.h"

#include "reader.h"

#include <algorithm>

namespace genome
{
namespace
{

// The header is a fixed-size struct, not a walked one: every field below sits at
// a constant offset and the payload always starts here.
constexpr std::size_t c_PixelDataOffset = 87;

std::size_t bytesPerPixel(ImageFormat format)
{
    switch (format)
    {
        case ImageFormat::R8G8B8: return 3;
        case ImageFormat::A8R8G8B8:
        case ImageFormat::X8R8G8B8: return 4;
        case ImageFormat::R5G6B5:
        case ImageFormat::X1R5G5B5:
        case ImageFormat::A1R5G5B5:
        case ImageFormat::A4R4G4B4:
        case ImageFormat::A8L8: return 2;
        case ImageFormat::A8:
        case ImageFormat::L8: return 1;
        default: return 0;
    }
}

// Red, green, blue, alpha. Kept as an array so the DXT interpolation can loop
// over the colour channels.
struct Texel
{
    std::uint8_t channel[4]{0, 0, 0, 255};
};

std::uint16_t read16(const std::uint8_t *at)
{
    return static_cast<std::uint16_t>(at[0] | (at[1] << 8));
}

std::uint32_t read32(const std::uint8_t *at)
{
    return static_cast<std::uint32_t>(at[0]) | (static_cast<std::uint32_t>(at[1]) << 8) |
           (static_cast<std::uint32_t>(at[2]) << 16) | (static_cast<std::uint32_t>(at[3]) << 24);
}

void expand565(std::uint16_t colour, Texel &texel)
{
    const std::uint32_t r = (colour >> 11) & 0x1F;
    const std::uint32_t g = (colour >> 5) & 0x3F;
    const std::uint32_t b = colour & 0x1F;
    texel.channel[0] = static_cast<std::uint8_t>((r << 3) | (r >> 2));
    texel.channel[1] = static_cast<std::uint8_t>((g << 2) | (g >> 4));
    texel.channel[2] = static_cast<std::uint8_t>((b << 3) | (b >> 2));
    texel.channel[3] = 255;
}

std::uint8_t blend(std::uint8_t first, std::uint8_t second, int firstWeight, int secondWeight, int total)
{
    return static_cast<std::uint8_t>((first * firstWeight + second * secondWeight) / total);
}

// The colour half of a DXT block. DXT1 spends the endpoint ordering on one bit
// of alpha; DXT2-5 carry alpha separately and are always in four-colour mode.
void decodeColourBlock(const std::uint8_t *block, bool oneBitAlpha, Texel texels[16])
{
    const std::uint16_t first = read16(block);
    const std::uint16_t second = read16(block + 2);
    const std::uint32_t indices = read32(block + 4);

    Texel palette[4];
    expand565(first, palette[0]);
    expand565(second, palette[1]);
    if (!oneBitAlpha || first > second)
    {
        for (int channel = 0; channel < 3; ++channel)
        {
            const std::uint8_t a = palette[0].channel[channel];
            const std::uint8_t b = palette[1].channel[channel];
            palette[2].channel[channel] = blend(a, b, 2, 1, 3);
            palette[3].channel[channel] = blend(a, b, 1, 2, 3);
        }
        palette[2].channel[3] = palette[3].channel[3] = 255;
    }
    else
    {
        for (int channel = 0; channel < 3; ++channel)
        {
            const std::uint8_t a = palette[0].channel[channel];
            const std::uint8_t b = palette[1].channel[channel];
            palette[2].channel[channel] = blend(a, b, 1, 1, 2);
            palette[3].channel[channel] = 0;
        }
        palette[2].channel[3] = 255;
        palette[3].channel[3] = 0; // the transparent slot, black by definition
    }

    for (int texel = 0; texel < 16; ++texel)
        texels[texel] = palette[(indices >> (2 * texel)) & 3];
}

// DXT2/3: four explicit alpha bits per texel.
void decodeAlphaNibbles(const std::uint8_t *block, Texel texels[16])
{
    for (int texel = 0; texel < 16; ++texel)
    {
        const std::uint8_t byte = block[texel / 2];
        const std::uint8_t nibble = (texel & 1) != 0 ? (byte >> 4) : (byte & 0x0F);
        texels[texel].channel[3] = static_cast<std::uint8_t>(nibble * 17); // 0..15 -> 0..255
    }
}

// DXT4/5: two endpoints plus three-bit indices, interpolated like the colours.
void decodeAlphaBlock(const std::uint8_t *block, Texel texels[16])
{
    std::uint8_t alpha[8];
    alpha[0] = block[0];
    alpha[1] = block[1];
    if (alpha[0] > alpha[1])
    {
        for (int step = 0; step < 6; ++step)
            alpha[2 + step] = blend(alpha[0], alpha[1], 6 - step, 1 + step, 7);
    }
    else
    {
        for (int step = 0; step < 4; ++step)
            alpha[2 + step] = blend(alpha[0], alpha[1], 4 - step, 1 + step, 5);
        alpha[6] = 0;
        alpha[7] = 255;
    }

    std::uint64_t indices = 0;
    for (int byte = 0; byte < 6; ++byte)
        indices |= static_cast<std::uint64_t>(block[2 + byte]) << (8 * byte);
    for (int texel = 0; texel < 16; ++texel)
        texels[texel].channel[3] = alpha[(indices >> (3 * texel)) & 7];
}

void readPixel(ImageFormat format, const std::uint8_t *at, Texel &texel)
{
    switch (format)
    {
        case ImageFormat::R8G8B8: // D3DCOLOR order: blue first in memory
            texel = {{at[2], at[1], at[0], 255}};
            break;
        case ImageFormat::A8R8G8B8:
            texel = {{at[2], at[1], at[0], at[3]}};
            break;
        case ImageFormat::X8R8G8B8:
            texel = {{at[2], at[1], at[0], 255}};
            break;
        case ImageFormat::R5G6B5:
            expand565(read16(at), texel);
            break;
        case ImageFormat::X1R5G5B5:
        case ImageFormat::A1R5G5B5:
        {
            const std::uint16_t value = read16(at);
            const std::uint32_t r = (value >> 10) & 0x1F, g = (value >> 5) & 0x1F, b = value & 0x1F;
            texel.channel[0] = static_cast<std::uint8_t>((r << 3) | (r >> 2));
            texel.channel[1] = static_cast<std::uint8_t>((g << 3) | (g >> 2));
            texel.channel[2] = static_cast<std::uint8_t>((b << 3) | (b >> 2));
            texel.channel[3] = format == ImageFormat::A1R5G5B5 && (value & 0x8000) == 0 ? 0 : 255;
            break;
        }
        case ImageFormat::A4R4G4B4:
        {
            const std::uint16_t value = read16(at);
            texel.channel[0] = static_cast<std::uint8_t>(((value >> 8) & 0x0F) * 17);
            texel.channel[1] = static_cast<std::uint8_t>(((value >> 4) & 0x0F) * 17);
            texel.channel[2] = static_cast<std::uint8_t>((value & 0x0F) * 17);
            texel.channel[3] = static_cast<std::uint8_t>(((value >> 12) & 0x0F) * 17);
            break;
        }
        case ImageFormat::A8:
            texel = {{255, 255, 255, at[0]}};
            break;
        case ImageFormat::L8:
            texel = {{at[0], at[0], at[0], 255}};
            break;
        case ImageFormat::A8L8:
            texel = {{at[0], at[0], at[0], at[1]}};
            break;
        default:
            break;
    }
}

} // namespace

const char *formatName(ImageFormat format)
{
    switch (format)
    {
        case ImageFormat::R8G8B8: return "R8G8B8";
        case ImageFormat::A8R8G8B8: return "A8R8G8B8";
        case ImageFormat::X8R8G8B8: return "X8R8G8B8";
        case ImageFormat::R5G6B5: return "R5G6B5";
        case ImageFormat::X1R5G5B5: return "X1R5G5B5";
        case ImageFormat::A1R5G5B5: return "A1R5G5B5";
        case ImageFormat::A4R4G4B4: return "A4R4G4B4";
        case ImageFormat::A8: return "A8";
        case ImageFormat::L8: return "L8";
        case ImageFormat::A8L8: return "A8L8";
        case ImageFormat::Dxt1: return "DXT1";
        case ImageFormat::Dxt2: return "DXT2";
        case ImageFormat::Dxt3: return "DXT3";
        case ImageFormat::Dxt4: return "DXT4";
        case ImageFormat::Dxt5: return "DXT5";
        default: return "unknown";
    }
}

bool isCompressed(ImageFormat format)
{
    switch (format)
    {
        case ImageFormat::Dxt1:
        case ImageFormat::Dxt2:
        case ImageFormat::Dxt3:
        case ImageFormat::Dxt4:
        case ImageFormat::Dxt5: return true;
        default: return false;
    }
}

std::size_t levelSize(ImageFormat format, std::uint32_t width, std::uint32_t height)
{
    if (width == 0 || height == 0)
        return 0;
    if (isCompressed(format))
    {
        const std::size_t blocks = static_cast<std::size_t>((width + 3) / 4) * ((height + 3) / 4);
        return blocks * (format == ImageFormat::Dxt1 ? 8 : 16);
    }
    const std::size_t stride = bytesPerPixel(format);
    return stride * width * height;
}

ImageLevel Image::level(std::uint32_t mip, std::uint32_t face) const
{
    if (mip >= levels.size() || face >= faceCount)
        return {};
    ImageLevel level = levels[mip];
    level.offset += static_cast<std::size_t>(face) * faceStride;
    return level;
}

bool loadImage(const std::vector<std::uint8_t> &bytes, Image &image, std::string *error)
{
    const auto fail = [&](const char *reason) {
        if (error)
            *error = reason;
        return false;
    };

    Reader reader(bytes);
    if (!reader.match("GENOMFLE", 8))
        return fail("not a Genome file");
    reader.skip(8);
    if (reader.u16() != 1)
        return fail("unknown container version");

    const std::uint32_t dataEnd = reader.u32();
    reader.u16();                         // class version, 32 throughout
    reader.u32();                         // logical size, unrounded and informational
    reader.u32();                         // uninitialised heap: never validate against it
    image.sourceTime = reader.u64();
    image.sourceSize = reader.u32();
    reader.u16();                         // always 1
    if (!reader.match("G3IMG", 5))
        return fail("not an image block");
    reader.skip(5);
    reader.u16();                         // image block version, 2 throughout
    reader.u16();                         // unknown, garbage in some files
    image.width = reader.u32();
    image.height = reader.u32();
    reader.u32();                         // depth, zero throughout
    const std::uint8_t cubeMap = reader.u8();
    reader.skip(3);                       // uninitialised padding, ASCII fragments in some files
    const std::uint32_t mipCount = reader.u32();
    reader.u32();                         // zero throughout
    image.format = static_cast<ImageFormat>(reader.u32());
    reader.u32();                         // always 1
    reader.u32();                         // logical size again
    reader.u32();                         // zero throughout

    if (!reader.ok())
        return fail("truncated header");
    if (reader.tell() != c_PixelDataOffset)
        return fail("header is not the fixed 87 bytes");
    if (dataEnd < c_PixelDataOffset || dataEnd > bytes.size())
        return fail("payload runs past the end of the file");
    if (image.width == 0 || image.height == 0 || image.width > 16384 || image.height > 16384)
        return fail("implausible dimensions");
    if (mipCount == 0 || mipCount > 16)
        return fail("implausible mip count");
    if (levelSize(image.format, image.width, image.height) == 0)
        return fail("unsupported pixel format");

    image.faceCount = cubeMap != 0 ? 6 : 1;

    // Rebuild the level table. The chain runs backwards - the smallest mip is
    // first and level 0 ends exactly at dataEnd - so sizes are accumulated from
    // the end of the face.
    image.levels.assign(mipCount, ImageLevel{});
    std::size_t chain = 0;
    for (std::uint32_t mip = 0; mip < mipCount; ++mip)
    {
        ImageLevel &level = image.levels[mip];
        level.width = std::max(1u, image.width >> mip);
        level.height = std::max(1u, image.height >> mip);
        level.size = levelSize(image.format, level.width, level.height);
        chain += level.size;
    }
    std::size_t at = chain;
    for (std::uint32_t mip = 0; mip < mipCount; ++mip)
    {
        at -= image.levels[mip].size;
        image.levels[mip].offset = at;
    }

    const std::size_t payload = dataEnd - c_PixelDataOffset;
    if (chain * image.faceCount != payload)
        return fail("mip chain does not fill the payload");

    // The payload is followed by DEADBEEF and an empty string table. Checking it
    // is what turns "the sizes happen to add up" into "we read the right bytes".
    Reader tail(bytes);
    tail.seek(dataEnd);
    if (!tail.match("\xEF\xBE\xAD\xDE", 4))
        return fail("payload does not end at the file's own marker");

    image.faceStride = chain;
    image.data.assign(bytes.begin() + c_PixelDataOffset, bytes.begin() + dataEnd);
    return true;
}

bool decodeLevel(const Image &image, std::uint32_t mip, std::uint32_t face, std::vector<std::uint8_t> &rgba,
                 std::string *error)
{
    const auto fail = [&](const char *reason) {
        if (error)
            *error = reason;
        return false;
    };

    const ImageLevel level = image.level(mip, face);
    if (level.size == 0)
        return fail("no such mip level");
    if (level.offset + level.size > image.data.size())
        return fail("level runs past the payload");

    rgba.assign(static_cast<std::size_t>(level.width) * level.height * 4, 0);
    const std::uint8_t *source = image.levelData(level);

    const auto store = [&](std::uint32_t x, std::uint32_t y, const Texel &texel) {
        std::uint8_t *out = rgba.data() + (static_cast<std::size_t>(y) * level.width + x) * 4;
        for (int channel = 0; channel < 4; ++channel)
            out[channel] = texel.channel[channel];
    };

    if (isCompressed(image.format))
    {
        const std::size_t blockBytes = image.format == ImageFormat::Dxt1 ? 8 : 16;
        const std::uint32_t blocksX = (level.width + 3) / 4;
        const std::uint32_t blocksY = (level.height + 3) / 4;
        for (std::uint32_t blockY = 0; blockY < blocksY; ++blockY)
        {
            for (std::uint32_t blockX = 0; blockX < blocksX; ++blockX)
            {
                const std::uint8_t *block = source + (static_cast<std::size_t>(blockY) * blocksX + blockX) * blockBytes;
                Texel texels[16];
                switch (image.format)
                {
                    case ImageFormat::Dxt1:
                        decodeColourBlock(block, true, texels);
                        break;
                    case ImageFormat::Dxt2:
                    case ImageFormat::Dxt3:
                        decodeColourBlock(block + 8, false, texels);
                        decodeAlphaNibbles(block, texels);
                        break;
                    default:
                        decodeColourBlock(block + 8, false, texels);
                        decodeAlphaBlock(block, texels);
                        break;
                }

                // The last block of a non-multiple-of-four level hangs over the
                // edge; the extra texels are decoded and dropped.
                for (std::uint32_t y = 0; y < 4; ++y)
                {
                    for (std::uint32_t x = 0; x < 4; ++x)
                    {
                        const std::uint32_t px = blockX * 4 + x, py = blockY * 4 + y;
                        if (px < level.width && py < level.height)
                            store(px, py, texels[y * 4 + x]);
                    }
                }
            }
        }
        return true;
    }

    const std::size_t stride = bytesPerPixel(image.format);
    if (stride == 0)
        return fail("unsupported pixel format");
    for (std::uint32_t y = 0; y < level.height; ++y)
    {
        for (std::uint32_t x = 0; x < level.width; ++x)
        {
            Texel texel;
            readPixel(image.format, source + (static_cast<std::size_t>(y) * level.width + x) * stride, texel);
            store(x, y, texel);
        }
    }
    return true;
}

} // namespace genome
