#include "physics/world.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace physics
{
namespace
{

// Eight triangles a leaf. Small enough that a leaf is mostly hits, large enough
// that the tree does not cost more to walk than the triangles cost to test.
constexpr std::uint32_t c_LeafSize = 8;

// The engine's convention, and the vertex shader's: the rows apply directly and
// the fourth is the translation.
std::array<float, 3> place(const Matrix &m, const std::array<float, 3> &p)
{
    return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
            m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
}

// The linear part, as a plain 3x3 in row-major order: world = A * local + t.
std::array<float, 9> linearOf(const Matrix &m)
{
    return {m[0], m[4], m[8], m[1], m[5], m[9], m[2], m[6], m[10]};
}

// Cofactor inverse. Returns false for a degenerate matrix, which does happen -
// a placement scaled to nothing on one axis is in the shipping data.
bool invert(const std::array<float, 9> &a, std::array<float, 9> &out)
{
    const float c00 = a[4] * a[8] - a[5] * a[7];
    const float c01 = a[5] * a[6] - a[3] * a[8];
    const float c02 = a[3] * a[7] - a[4] * a[6];
    const float determinant = a[0] * c00 + a[1] * c01 + a[2] * c02;
    if (std::fabs(determinant) < 1e-20f)
        return false;
    const float scale = 1.0f / determinant;
    out = {c00 * scale,
           (a[2] * a[7] - a[1] * a[8]) * scale,
           (a[1] * a[5] - a[2] * a[4]) * scale,
           c01 * scale,
           (a[0] * a[8] - a[2] * a[6]) * scale,
           (a[2] * a[3] - a[0] * a[5]) * scale,
           c02 * scale,
           (a[1] * a[6] - a[0] * a[7]) * scale,
           (a[0] * a[4] - a[1] * a[3]) * scale};
    return true;
}

std::array<float, 3> apply(const std::array<float, 9> &a, const std::array<float, 3> &p)
{
    return {a[0] * p[0] + a[1] * p[1] + a[2] * p[2], a[3] * p[0] + a[4] * p[1] + a[5] * p[2],
            a[6] * p[0] + a[7] * p[1] + a[8] * p[2]};
}

// Moller-Trumbore. The parameter it returns is along the ray as given, so with
// a world-space direction carried into local space unnormalised, it comes back
// as the world distance.
bool hitTriangle(const std::array<float, 3> &origin, const std::array<float, 3> &direction, const float *t,
                 float &distance)
{
    const std::array<float, 3> ab{t[3] - t[0], t[4] - t[1], t[5] - t[2]};
    const std::array<float, 3> ac{t[6] - t[0], t[7] - t[1], t[8] - t[2]};
    const std::array<float, 3> p{direction[1] * ac[2] - direction[2] * ac[1],
                                 direction[2] * ac[0] - direction[0] * ac[2],
                                 direction[0] * ac[1] - direction[1] * ac[0]};
    const float determinant = ab[0] * p[0] + ab[1] * p[1] + ab[2] * p[2];
    // Both faces count: a floor met from below is still a floor, and the
    // shipping meshes are not consistently wound.
    if (std::fabs(determinant) < 1e-12f)
        return false;
    const float scale = 1.0f / determinant;

    const std::array<float, 3> offset{origin[0] - t[0], origin[1] - t[1], origin[2] - t[2]};
    const float u = (offset[0] * p[0] + offset[1] * p[1] + offset[2] * p[2]) * scale;
    if (u < 0.0f || u > 1.0f)
        return false;

    const std::array<float, 3> q{offset[1] * ab[2] - offset[2] * ab[1], offset[2] * ab[0] - offset[0] * ab[2],
                                 offset[0] * ab[1] - offset[1] * ab[0]};
    const float v = (direction[0] * q[0] + direction[1] * q[1] + direction[2] * q[2]) * scale;
    if (v < 0.0f || u + v > 1.0f)
        return false;

    distance = (ac[0] * q[0] + ac[1] * q[1] + ac[2] * q[2]) * scale;
    return distance >= 0.0f;
}

// The slab test, with the reciprocal computed once by the caller. Infinities
// behave here: a ray parallel to an axis gets one, and the comparisons still
// decide it correctly, so that axis needs no special case.
bool hitBounds(const std::array<float, 6> &bounds, const std::array<float, 3> &origin,
               const std::array<float, 3> &recip, float limit)
{
    float near = 0.0f, far = limit;
    for (int axis = 0; axis < 3; ++axis)
    {
        float low = (bounds[axis] - origin[axis]) * recip[axis];
        float high = (bounds[axis + 3] - origin[axis]) * recip[axis];
        if (low > high)
            std::swap(low, high);
        near = low > near ? low : near;
        far = high < far ? high : far;
        if (near > far)
            return false;
    }
    return true;
}

// Median split on the widest axis. Not the best tree money can buy - a surface
// area heuristic would be better - but it is built once per mesh, and the win
// over testing every triangle is the whole of the difference.
std::uint32_t buildNode(MeshIndex &index, std::vector<std::uint32_t> &order, const std::vector<float> &source,
                        std::uint32_t first, std::uint32_t count)
{
    const std::uint32_t self = std::uint32_t(index.nodes.size());
    index.nodes.emplace_back();

    std::array<float, 6> bounds{std::numeric_limits<float>::max(),  std::numeric_limits<float>::max(),
                                std::numeric_limits<float>::max(),  -std::numeric_limits<float>::max(),
                                -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
    for (std::uint32_t at = first; at < first + count; ++at)
    {
        const float *t = &source[std::size_t(order[at]) * 9];
        for (int corner = 0; corner < 3; ++corner)
            for (int axis = 0; axis < 3; ++axis)
            {
                bounds[axis] = std::min(bounds[axis], t[corner * 3 + axis]);
                bounds[axis + 3] = std::max(bounds[axis + 3], t[corner * 3 + axis]);
            }
    }
    index.nodes[self].bounds = bounds;

    if (count <= c_LeafSize)
    {
        index.nodes[self].first = std::uint32_t(index.triangles.size() / 9);
        index.nodes[self].count = count;
        for (std::uint32_t at = first; at < first + count; ++at)
        {
            const float *t = &source[std::size_t(order[at]) * 9];
            index.triangles.insert(index.triangles.end(), t, t + 9);
        }
        return self;
    }

    int axis = 0;
    float widest = 0.0f;
    for (int candidate = 0; candidate < 3; ++candidate)
    {
        const float width = bounds[candidate + 3] - bounds[candidate];
        if (width > widest)
        {
            widest = width;
            axis = candidate;
        }
    }

    const auto centroid = [&](std::uint32_t triangle) {
        const float *t = &source[std::size_t(triangle) * 9];
        return t[axis] + t[3 + axis] + t[6 + axis];
    };
    const std::uint32_t middle = first + count / 2;
    std::nth_element(order.begin() + first, order.begin() + middle, order.begin() + first + count,
                     [&](std::uint32_t a, std::uint32_t b) { return centroid(a) < centroid(b); });

    // The left child follows this node; the right one is built after the whole
    // left subtree, so its index is remembered rather than implied.
    buildNode(index, order, source, first, middle - first);
    const std::uint32_t right = buildNode(index, order, source, middle, first + count - middle);
    index.nodes[self].child = right;
    index.nodes[self].count = 0;
    return self;
}

} // namespace

const MeshIndex *CollisionWorld::indexFor(const genome::Mesh &mesh)
{
    const auto known = m_indexOf.find(&mesh);
    if (known != m_indexOf.end())
        return known->second.get();

    std::vector<float> source;
    for (const genome::MeshElement &element : mesh.elements)
    {
        for (std::size_t at = 0; at + 2 < element.indices.size(); at += 3)
        {
            const std::uint32_t a = element.indices[at];
            const std::uint32_t b = element.indices[at + 1];
            const std::uint32_t c = element.indices[at + 2];
            if (a >= element.positions.size() || b >= element.positions.size() || c >= element.positions.size())
                continue;
            for (std::uint32_t corner : {a, b, c})
                source.insert(source.end(), element.positions[corner].begin(), element.positions[corner].end());
        }
    }

    auto index = std::make_unique<MeshIndex>();
    if (!source.empty())
    {
        std::vector<std::uint32_t> order(source.size() / 9);
        std::iota(order.begin(), order.end(), 0u);
        index->triangles.reserve(source.size());
        buildNode(*index, order, source, 0, std::uint32_t(order.size()));
    }

    const MeshIndex *made = index.get();
    m_indexOf.emplace(&mesh, std::move(index));
    return made;
}

std::size_t CollisionWorld::distinctTriangles() const
{
    std::size_t total = 0;
    for (const auto &entry : m_indexOf)
        total += entry.second->triangleCount();
    return total;
}

void CollisionWorld::forget()
{
    m_indexOf.clear();
}

void CollisionWorld::clear()
{
    m_instances.clear();
    m_cells.clear();
    m_triangles = 0;
    m_references = 0;
}

void CollisionWorld::add(const genome::Mesh &mesh, const Matrix &world)
{
    Instance instance;
    instance.world = world;
    instance.translation = {world[12], world[13], world[14]};
    if (!invert(linearOf(world), instance.inverse))
        return; // scaled flat on an axis; nothing to collide with

    instance.index = indexFor(mesh);
    if (instance.index->nodes.empty())
        return;
    m_triangles += instance.index->triangleCount();

    // The tree's root box placed by its eight corners: cheaper than walking
    // every vertex, and the result is that box or a little larger, which only
    // ever costs a query a look.
    const std::array<float, 6> local = instance.index->nodes[0].bounds;
    instance.bounds = {std::numeric_limits<float>::max(),  std::numeric_limits<float>::max(),
                       std::numeric_limits<float>::max(),  -std::numeric_limits<float>::max(),
                       -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
    for (int corner = 0; corner < 8; ++corner)
    {
        const std::array<float, 3> at =
            place(world, {(corner & 1) ? local[3] : local[0], (corner & 2) ? local[4] : local[1],
                          (corner & 4) ? local[5] : local[2]});
        for (int axis = 0; axis < 3; ++axis)
        {
            instance.bounds[axis] = std::min(instance.bounds[axis], at[axis]);
            instance.bounds[axis + 3] = std::max(instance.bounds[axis + 3], at[axis]);
        }
    }
    m_instances.push_back(instance);
}

void CollisionWorld::build()
{
    m_cells.clear();
    m_references = 0;

    for (std::uint32_t index = 0; index < m_instances.size(); ++index)
    {
        const Instance &instance = m_instances[index];
        const std::int32_t firstX = std::int32_t(std::floor(instance.bounds[0] / c_CellSize));
        const std::int32_t lastX = std::int32_t(std::floor(instance.bounds[3] / c_CellSize));
        const std::int32_t firstZ = std::int32_t(std::floor(instance.bounds[2] / c_CellSize));
        const std::int32_t lastZ = std::int32_t(std::floor(instance.bounds[5] / c_CellSize));
        if (lastX < firstX || lastZ < firstZ || lastX - firstX > 256 || lastZ - firstZ > 256)
            continue;

        for (std::int32_t x = firstX; x <= lastX; ++x)
            for (std::int32_t z = firstZ; z <= lastZ; ++z)
            {
                m_cells[Cell{x, z}].push_back(index);
                ++m_references;
            }
    }
}

bool CollisionWorld::groundBelow(const std::array<float, 3> &at, float reach, float &height,
                                 std::array<float, 3> *normal) const
{
    m_visited = 0;
    m_tested = 0;
    const auto cell = m_cells.find(
        Cell{std::int32_t(std::floor(at[0] / c_CellSize)), std::int32_t(std::floor(at[2] / c_CellSize))});
    if (cell == m_cells.end())
        return false;

    const std::array<float, 3> down{0.0f, -1.0f, 0.0f};
    float best = reach;
    bool found = false;
    std::array<float, 3> hitCorners[3];
    std::uint32_t stack[64];

    for (std::uint32_t index : cell->second)
    {
        const Instance &instance = m_instances[index];
        // Reject only what the ray cannot reach: an instance entirely above the
        // start, or entirely below the end. Comparing the wrong end of the box
        // here threw away every instance that merely reaches above the probe -
        // which is the walls of the room the floor belongs to, so a probe from
        // the sky worked and one from the feet found nothing at all.
        if (at[0] < instance.bounds[0] || at[0] > instance.bounds[3] || at[2] < instance.bounds[2] ||
            at[2] > instance.bounds[5] || instance.bounds[1] > at[1] || instance.bounds[4] < at[1] - reach)
            continue;
        ++m_visited;

        const std::array<float, 3> offset{at[0] - instance.translation[0], at[1] - instance.translation[1],
                                          at[2] - instance.translation[2]};
        const std::array<float, 3> origin = apply(instance.inverse, offset);
        const std::array<float, 3> direction = apply(instance.inverse, down);
        const std::array<float, 3> recip{1.0f / direction[0], 1.0f / direction[1], 1.0f / direction[2]};

        const MeshIndex &tree = *instance.index;
        std::size_t depth = 0;
        stack[depth++] = 0;
        while (depth != 0)
        {
            const std::uint32_t which = stack[--depth];
            const MeshIndex::Node &node = tree.nodes[which];
            if (!hitBounds(node.bounds, origin, recip, best))
                continue;

            if (node.count == 0)
            {
                if (depth + 2 <= std::size(stack))
                {
                    stack[depth++] = node.child; // the right subtree
                    stack[depth++] = which + 1;  // the left, which follows this node
                }
                continue;
            }

            for (std::uint32_t triangle = 0; triangle < node.count; ++triangle)
            {
                const float *t = &tree.triangles[(std::size_t(node.first) + triangle) * 9];
                ++m_tested;
                float distance = 0.0f;
                if (!hitTriangle(origin, direction, t, distance) || distance >= best)
                    continue;

                best = distance;
                found = true;
                if (normal != nullptr)
                    for (int corner = 0; corner < 3; ++corner)
                        hitCorners[corner] =
                            place(instance.world, {t[corner * 3], t[corner * 3 + 1], t[corner * 3 + 2]});
            }
        }
    }

    if (!found)
        return false;
    height = at[1] - best;

    if (normal != nullptr)
    {
        const std::array<float, 3> u{hitCorners[1][0] - hitCorners[0][0], hitCorners[1][1] - hitCorners[0][1],
                                     hitCorners[1][2] - hitCorners[0][2]};
        const std::array<float, 3> v{hitCorners[2][0] - hitCorners[0][0], hitCorners[2][1] - hitCorners[0][1],
                                     hitCorners[2][2] - hitCorners[0][2]};
        std::array<float, 3> n{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (length > 1e-9f)
            for (float &component : n)
                component /= length;
        // Ground faces up; the same surface wound the other way is still ground.
        if (n[1] < 0.0f)
            for (float &component : n)
                component = -component;
        *normal = n;
    }
    return true;
}

} // namespace physics
