#pragma once

// Compiled texture (.ximg): a GENOMFLE wrapper whose payload is not a property
// set but a raw eCResourceImage block - a fixed 87-byte header followed by a
// D3D mip chain.
//
// Two things about that chain are unusual. It is stored SMALLEST FIRST, so mip
// level 0 is the last block in the file and reading forward as if it were a DDS
// gives a garbage image at every level; and it is often truncated, so the stored
// mip count is the truth and log2(max(w, h)) + 1 is not.
//
// The blocks themselves are byte-identical to BC1/BC2/BC3, which is why nothing
// here decompresses by default: a level is handed out exactly as it sits on
// disk, ready to be copied into a GPU image. The CPU decode below exists for
// tooling and screenshots.

#include <cstdint>
#include <string>
#include <vector>

namespace genome
{

// The header's format field is a full D3DFORMAT, so FourCCs and small D3DFMT_*
// integers share one space. Shipping data only uses DXT1/3/5 and A8R8G8B8, but
// the other uncompressed shapes cost nothing to support and mods may use them.
enum class ImageFormat : std::uint32_t
{
    Unknown = 0,
    R8G8B8 = 20,
    A8R8G8B8 = 21,
    X8R8G8B8 = 22,
    R5G6B5 = 23,
    X1R5G5B5 = 24,
    A1R5G5B5 = 25,
    A4R4G4B4 = 26,
    A8 = 28,
    L8 = 50,
    A8L8 = 51,
    Dxt1 = 0x31545844, // 'DXT1'
    Dxt2 = 0x32545844,
    Dxt3 = 0x33545844,
    Dxt4 = 0x34545844,
    Dxt5 = 0x35545844,
};

const char *formatName(ImageFormat format);

// True for the DXT formats, i.e. the ones whose level data is 4x4 blocks and
// can go to the GPU untouched.
bool isCompressed(ImageFormat format);

// Bytes one mip level occupies: rounded-up 4x4 blocks for the DXT formats,
// whole pixels otherwise. Zero for a format we cannot size, which is how an
// unknown format is rejected before it can be mistaken for valid data.
std::size_t levelSize(ImageFormat format, std::uint32_t width, std::uint32_t height);

struct ImageLevel
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t offset = 0; // into Image::data
    std::size_t size = 0;
};

struct Image
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t faceCount = 1; // 6 for a cube map
    ImageFormat format = ImageFormat::Unknown;
    std::uint32_t sourceSize = 0; // size of the .tga/.dds/.bmp the compiler ate
    std::uint64_t sourceTime = 0; // its Windows FILETIME

    // The payload exactly as stored, smallest mip first and, for a cube map, one
    // whole chain per face. `levels` re-indexes it with level 0 first - the
    // order every GPU API wants - so uploading is a walk over `levels` with no
    // reshuffling of the bytes.
    std::vector<std::uint8_t> data;
    std::vector<ImageLevel> levels; // face 0; use level() for the other faces
    std::size_t faceStride = 0;     // bytes per face

    std::uint32_t mipCount() const { return static_cast<std::uint32_t>(levels.size()); }

    // Level `mip` of `face`, level 0 being full resolution. Faces are stored
    // face-major - one whole chain each - which decoding both shipping cube maps
    // confirms: all twelve faces come out as coherent images that way. Which
    // face is +X is still only assumed to follow the D3D order.
    ImageLevel level(std::uint32_t mip, std::uint32_t face = 0) const;
    const std::uint8_t *levelData(const ImageLevel &level) const { return data.data() + level.offset; }
};

// Parses a whole .ximg file. Returns false and fills `error` on malformed data.
bool loadImage(const std::vector<std::uint8_t> &bytes, Image &image, std::string *error = nullptr);

// CPU decode of one level to 8-bit RGBA, row-major with the origin top-left.
// This is for tooling; a renderer uploads the stored blocks instead.
bool decodeLevel(const Image &image, std::uint32_t mip, std::uint32_t face, std::vector<std::uint8_t> &rgba,
                 std::string *error = nullptr);

} // namespace genome
