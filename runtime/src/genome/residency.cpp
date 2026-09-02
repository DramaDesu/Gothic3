#include "residency.h"

#include "reader.h"

#include <algorithm>
#include <cmath>

namespace genome
{
namespace
{

// The box sits at a fixed place in a fixed-size file. It appears twice - the
// property's default and its value - and the two are identical in the shipping
// data, so the first is taken.
constexpr std::size_t c_BoundsOffset = 0x4F;
constexpr std::size_t c_FileSize = 197;

} // namespace

bool loadSectorBounds(const std::vector<std::uint8_t> &bytes, SectorBounds &out)
{
    if (bytes.size() < c_BoundsOffset + 24)
        return false;

    Reader reader(bytes);
    reader.seek(c_BoundsOffset);
    reader.array(out.min.data(), 3);
    reader.array(out.max.data(), 3);
    if (!reader.ok())
        return false;

    // An inverted box is how an empty sector says so: 66 of the 2177 carry
    // (+FLT_MAX...) .. (-FLT_MAX...), which is a box initialised and never
    // grown. Those sectors hold nothing to draw, so refusing them here is the
    // right answer rather than a parse failure.
    for (int axis = 0; axis < 3; ++axis)
        if (!(out.max[axis] >= out.min[axis]))
            return false;
    return true;
}

bool ResidentCells::operator==(const ResidentCells &other) const
{
    return left == other.left && top == other.top && right == other.right && bottom == other.bottom;
}

int cellIndex(float worldCoordinate)
{
    return int((int(worldCoordinate) + int(c_CellBias)) / int(c_CellSize));
}

ResidentCells residentCells(const std::array<float, 3> &eye, float farPlane)
{
    // Four tenths of the far plane, and a cell of margin on each side - the
    // margin is what stops a sector popping in exactly as it is needed.
    const int reach = int(farPlane * 0.4f);
    const int x = int(eye[0]) + int(c_CellBias);
    const int z = int(eye[2]) + int(c_CellBias);
    const int size = int(c_CellSize);

    ResidentCells cells;
    cells.left = (x - reach) / size - 1;
    cells.top = (z - reach) / size - 1;
    cells.right = (x + reach) / size + 1;
    cells.bottom = (z + reach) / size + 1;
    return cells;
}

bool overlaps(const SectorBounds &bounds, const ResidentCells &cells)
{
    const int left = cellIndex(bounds.min[0]);
    const int right = cellIndex(bounds.max[0]);
    const int top = cellIndex(bounds.min[2]);
    const int bottom = cellIndex(bounds.max[2]);
    return left <= cells.right && right >= cells.left && top <= cells.bottom && bottom >= cells.top;
}

} // namespace genome
