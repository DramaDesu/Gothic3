#include "occlusion.h"

#include <algorithm>
#include <cmath>

namespace render
{

OcclusionBuffer::OcclusionBuffer(int width, int height) : m_width(width), m_height(height), m_depth(width * height, 1.0f)
{}

void OcclusionBuffer::clear()
{
    std::fill(m_depth.begin(), m_depth.end(), 1.0f);
}

bool OcclusionBuffer::project(const std::array<float, 6> &bounds, const std::array<float, 16> &m, int &minX, int &minY,
                              int &maxX, int &maxY, float &nearDepth, float &farDepth) const
{
    float left = 1e30f, right = -1e30f, top = 1e30f, bottom = -1e30f;
    nearDepth = 1e30f;
    farDepth = -1e30f;

    for (int corner = 0; corner < 8; ++corner)
    {
        const float x = (corner & 1) ? bounds[3] : bounds[0];
        const float y = (corner & 2) ? bounds[4] : bounds[1];
        const float z = (corner & 4) ? bounds[5] : bounds[2];

        const float clipX = m[0] * x + m[4] * y + m[8] * z + m[12];
        const float clipY = m[1] * x + m[5] * y + m[9] * z + m[13];
        const float clipZ = m[2] * x + m[6] * y + m[10] * z + m[14];
        const float clipW = m[3] * x + m[7] * y + m[11] * z + m[15];

        // Anything crossing the camera plane is not projectable; treat the whole
        // box as unknown rather than guessing.
        if (clipW <= 1e-4f)
            return false;

        const float screenX = (clipX / clipW * 0.5f + 0.5f) * float(m_width);
        const float screenY = (clipY / clipW * 0.5f + 0.5f) * float(m_height);
        const float depth = clipZ / clipW;

        left = std::min(left, screenX);
        right = std::max(right, screenX);
        top = std::min(top, screenY);
        bottom = std::max(bottom, screenY);
        nearDepth = std::min(nearDepth, depth);
        farDepth = std::max(farDepth, depth);
    }

    minX = std::max(0, int(std::floor(left)));
    minY = std::max(0, int(std::floor(top)));
    maxX = std::min(m_width - 1, int(std::ceil(right)));
    maxY = std::min(m_height - 1, int(std::ceil(bottom)));
    return minX <= maxX && minY <= maxY;
}

void OcclusionBuffer::addOccluder(const std::array<float, 6> &bounds, const std::array<float, 16> &viewProjection)
{
    int minX = 0, minY = 0, maxX = 0, maxY = 0;
    float nearDepth = 0.0f, farDepth = 0.0f;
    if (!project(bounds, viewProjection, minX, minY, maxX, maxY, nearDepth, farDepth))
        return;

    // The far face: a box is a loose fit around its object, so claiming the near
    // depth would hide things the object does not actually cover.
    for (int y = minY; y <= maxY; ++y)
    {
        float *row = m_depth.data() + std::size_t(y) * m_width;
        for (int x = minX; x <= maxX; ++x)
            row[x] = std::min(row[x], farDepth);
    }
}

bool OcclusionBuffer::isOccluded(const std::array<float, 6> &bounds, const std::array<float, 16> &viewProjection) const
{
    int minX = 0, minY = 0, maxX = 0, maxY = 0;
    float nearDepth = 0.0f, farDepth = 0.0f;
    if (!project(bounds, viewProjection, minX, minY, maxX, maxY, nearDepth, farDepth))
        return false;

    // Hidden only if every pixel it could touch is already covered by something
    // closer than its nearest point.
    for (int y = minY; y <= maxY; ++y)
    {
        const float *row = m_depth.data() + std::size_t(y) * m_width;
        for (int x = minX; x <= maxX; ++x)
        {
            if (row[x] > nearDepth)
                return false;
        }
    }
    return true;
}

} // namespace render
