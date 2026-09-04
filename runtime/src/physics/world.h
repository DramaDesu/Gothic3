#pragma once

// The collision geometry as something to ask questions of, rather than to draw.
//
// The same shapes the collision view shows - cooked meshes, the primitives an
// entity authors on itself, the shapes a tree declares - indexed as instances
// rather than as triangles. Copying every instance's triangles into world space
// was tried first and is what this exists instead of: it came to 5.34 million
// triangles, 190 MB, and 611 ms to rebuild, for a world that streams. An
// instance is a pointer and a matrix, so the same set is 65 thousand of those,
// and a query goes the other way - into the instance's own space, where its
// triangles already are.

#include "genome/mesh.h"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace physics
{

using Matrix = std::array<float, 16>;

class CollisionWorld
{
  public:
    void clear();

    // One placement of a mesh. The matrix is the engine's own convention - row
    // major with the translation in the fourth row - so its rows apply the way
    // the vertex shader applies them. The mesh must outlive the world.
    void add(const genome::Mesh &mesh, const Matrix &world);

    // The grid over the instances, once they are all in.
    void build();

    // The ground under a point: the nearest surface below it, within reach.
    bool groundBelow(const std::array<float, 3> &at, float reach, float &height,
                     std::array<float, 3> *normal = nullptr) const;

    std::size_t instanceCount() const { return m_instances.size(); }
    std::size_t triangleCount() const { return m_triangles; }
    std::size_t cellCount() const { return m_cells.size(); }
    std::size_t referenceCount() const { return m_references; }
    // How many instances the last query had to look at, which is the number
    // that says whether the grid is doing its job.
    std::size_t lastVisited() const { return m_visited; }

  private:
    // Ten metres across. Small enough that a query touches few instances, large
    // enough that a castle wall does not land in a hundred cells.
    static constexpr float c_CellSize = 1000.0f;

    struct Instance
    {
        const genome::Mesh *mesh = nullptr;
        Matrix world{};
        // The 3x3 that undoes the matrix's linear part, and the translation it
        // undoes, so a query can be asked in the instance's own space.
        std::array<float, 9> inverse{};
        std::array<float, 3> translation{};
        std::array<float, 6> bounds{}; // world space: min then max
    };

    struct Cell
    {
        std::int32_t x = 0, z = 0;
        bool operator==(const Cell &other) const { return x == other.x && z == other.z; }
    };

    struct CellHash
    {
        std::size_t operator()(const Cell &cell) const
        {
            return std::size_t(std::uint32_t(cell.x) * 73856093u ^ std::uint32_t(cell.z) * 19349663u);
        }
    };

    // The local box of each distinct mesh, computed on its first placement.
    // The meshes built for collision carry no bounds of their own, and a
    // placement's box is what lets a query reject it without reading a triangle.
    std::unordered_map<const genome::Mesh *, std::array<float, 6>> m_localBounds;
    std::vector<Instance> m_instances;
    std::unordered_map<Cell, std::vector<std::uint32_t>, CellHash> m_cells;
    std::size_t m_triangles = 0;
    std::size_t m_references = 0;
    mutable std::size_t m_visited = 0;
};

} // namespace physics
