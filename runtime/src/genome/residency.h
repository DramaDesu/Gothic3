#pragma once

// Which sectors are close enough to be worth having.
//
// The game's own rule, read out of its prefetcher: a uniform grid of 10000-unit
// cells, a rectangle of them around the camera whose half-width is four tenths
// of the far clipping plane, one cell of margin on every side, and a sector is
// resident while its own bounding box - not its cell - overlaps that rectangle.
// The distinction matters: the box of the sector named x55000z55000 begins at
// x = 45000, a whole cell outside the name.
//
// That box lives beside each sector as a 197-byte .lrgeodat, six floats at
// offset 0x4F, so the whole residency index is 2177 small files rather than the
// 347 MB of sector data it decides about.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace genome
{

struct SectorBounds
{
    std::string path;                // the .node this describes
    std::array<float, 3> min{};
    std::array<float, 3> max{};
};

// Reads the six floats. False when the file is not the shape we expect.
bool loadSectorBounds(const std::vector<std::uint8_t> &bytes, SectorBounds &out);

// The rectangle of cells the camera keeps resident, as cell indices.
struct ResidentCells
{
    int left = 0, top = 0, right = 0, bottom = 0;

    bool contains(int x, int y) const { return x >= left && x <= right && y >= top && y <= bottom; }
    bool operator==(const ResidentCells &other) const;
};

// The engine biases every coordinate by a million so cell indices stay positive,
// and divides by the cell size. Both are its numbers, not ours.
constexpr float c_CellSize = 10000.0f;
constexpr float c_CellBias = 1000000.0f;

int cellIndex(float worldCoordinate);
ResidentCells residentCells(const std::array<float, 3> &eye, float farPlane);

// Whether a sector's box reaches into that rectangle.
bool overlaps(const SectorBounds &bounds, const ResidentCells &cells);

} // namespace genome
