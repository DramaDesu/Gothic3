#pragma once

// Software occlusion: a small depth buffer rasterised on the CPU from the few
// large objects that actually hide things, then used to reject the many small
// ones behind them.
//
// Frustum and screen-size tests cannot see walls: a chest inside a house is in
// front of the camera and large enough to matter, yet invisible. This is what
// removes it.

#include <array>
#include <cstdint>
#include <vector>

namespace render
{

class OcclusionBuffer
{
  public:
    OcclusionBuffer(int width = 256, int height = 144);

    void clear();

    // Rasterises a box as an occluder, using its FAR face so the occluder is
    // never deeper than the object it stands for - being conservative here is
    // what keeps visible things from vanishing.
    void addOccluder(const std::array<float, 6> &bounds, const std::array<float, 16> &viewProjection);

    // True when the box is certainly hidden. Uses the box's NEAR face, so
    // anything uncertain is reported visible.
    bool isOccluded(const std::array<float, 6> &bounds, const std::array<float, 16> &viewProjection) const;

  private:
    // Projects the eight corners and returns the screen rectangle plus the
    // near/far depths. Returns false when the box crosses behind the camera,
    // where the projection is not usable.
    bool project(const std::array<float, 6> &bounds, const std::array<float, 16> &viewProjection, int &minX, int &minY,
                 int &maxX, int &maxY, float &nearDepth, float &farDepth) const;

    int m_width;
    int m_height;
    std::vector<float> m_depth; // 1 = empty, smaller is closer
};

} // namespace render
