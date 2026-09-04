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

// The closest point of a triangle to a point. Ericson's routine: decide which
// feature owns the projection - a vertex, an edge, or the face - by the sign of
// the barycentric coordinates rather than by clamping and hoping.
std::array<float, 3> closestOnTriangle(const std::array<float, 3> &p, const std::array<float, 3> &a,
                                       const std::array<float, 3> &b, const std::array<float, 3> &c)
{
    const auto sub = [](const std::array<float, 3> &l, const std::array<float, 3> &r) {
        return std::array<float, 3>{l[0] - r[0], l[1] - r[1], l[2] - r[2]};
    };
    const auto dot = [](const std::array<float, 3> &l, const std::array<float, 3> &r) {
        return l[0] * r[0] + l[1] * r[1] + l[2] * r[2];
    };
    const auto step = [](const std::array<float, 3> &from, const std::array<float, 3> &along, float scale) {
        return std::array<float, 3>{from[0] + along[0] * scale, from[1] + along[1] * scale,
                                    from[2] + along[2] * scale};
    };

    const std::array<float, 3> ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f)
        return a;

    const std::array<float, 3> bp = sub(p, b);
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3)
        return b;

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
        return step(a, ab, d1 / (d1 - d3));

    const std::array<float, 3> cp = sub(p, c);
    const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6)
        return c;

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
        return step(a, ac, d2 / (d2 - d6));

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
        return step(b, sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6)));

    const float denominator = 1.0f / (va + vb + vc);
    return step(step(a, ab, vb * denominator), ac, vc * denominator);
}

// Where two segments come closest. Degenerate cases - either segment a point,
// or the two parallel - fall out of the clamping rather than needing a branch
// of their own.
void closestBetweenSegments(const std::array<float, 3> &p1, const std::array<float, 3> &q1,
                            const std::array<float, 3> &p2, const std::array<float, 3> &q2,
                            std::array<float, 3> &c1, std::array<float, 3> &c2)
{
    const std::array<float, 3> d1{q1[0] - p1[0], q1[1] - p1[1], q1[2] - p1[2]};
    const std::array<float, 3> d2{q2[0] - p2[0], q2[1] - p2[1], q2[2] - p2[2]};
    const std::array<float, 3> r{p1[0] - p2[0], p1[1] - p2[1], p1[2] - p2[2]};
    const auto dot = [](const std::array<float, 3> &l, const std::array<float, 3> &m) {
        return l[0] * m[0] + l[1] * m[1] + l[2] * m[2];
    };
    const float a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);

    float s = 0.0f, t = 0.0f;
    if (a <= 1e-12f && e <= 1e-12f)
    {
        c1 = p1;
        c2 = p2;
        return;
    }
    if (a <= 1e-12f)
        t = std::clamp(f / e, 0.0f, 1.0f);
    else
    {
        const float c = dot(d1, r);
        if (e <= 1e-12f)
            s = std::clamp(-c / a, 0.0f, 1.0f);
        else
        {
            const float b = dot(d1, d2);
            const float denominator = a * e - b * b;
            s = denominator > 1e-12f ? std::clamp((b * f - c * e) / denominator, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f)
            {
                t = 0.0f;
                s = std::clamp(-c / a, 0.0f, 1.0f);
            }
            else if (t > 1.0f)
            {
                t = 1.0f;
                s = std::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    c1 = {p1[0] + d1[0] * s, p1[1] + d1[1] * s, p1[2] + d1[2] * s};
    c2 = {p2[0] + d2[0] * t, p2[1] + d2[1] * t, p2[2] + d2[2] * t};
}

bool overlapBounds(const std::array<float, 6> &a, const std::array<float, 6> &b)
{
    return a[0] <= b[3] && a[3] >= b[0] && a[1] <= b[4] && a[4] >= b[1] && a[2] <= b[5] && a[5] >= b[2];
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


std::size_t CollisionWorld::capsuleContacts(const std::array<float, 3> &low, const std::array<float, 3> &high,
                                            float radius, std::vector<Contact> &out) const
{
    m_visited = 0;
    m_tested = 0;
    const std::size_t before = out.size();

    std::array<float, 6> query{std::min(low[0], high[0]) - radius, std::min(low[1], high[1]) - radius,
                               std::min(low[2], high[2]) - radius, std::max(low[0], high[0]) + radius,
                               std::max(low[1], high[1]) + radius, std::max(low[2], high[2]) + radius};

    std::uint32_t stack[64];
    // An instance can be in several of the cells the query covers, and a contact
    // counted twice pushes twice.
    std::vector<std::uint32_t> seen;

    const std::int32_t firstX = std::int32_t(std::floor(query[0] / c_CellSize));
    const std::int32_t lastX = std::int32_t(std::floor(query[3] / c_CellSize));
    const std::int32_t firstZ = std::int32_t(std::floor(query[2] / c_CellSize));
    const std::int32_t lastZ = std::int32_t(std::floor(query[5] / c_CellSize));

    for (std::int32_t x = firstX; x <= lastX; ++x)
        for (std::int32_t z = firstZ; z <= lastZ; ++z)
        {
            const auto cell = m_cells.find(Cell{x, z});
            if (cell == m_cells.end())
                continue;

            for (std::uint32_t index : cell->second)
            {
                if (std::find(seen.begin(), seen.end(), index) != seen.end())
                    continue;
                seen.push_back(index);

                const Instance &instance = m_instances[index];
                if (!overlapBounds(query, instance.bounds))
                    continue;
                ++m_visited;

                // The capsule carried into the instance's space. A radius does
                // not survive a non-uniform scale, so the local one is an upper
                // bound - the Frobenius norm bounds the operator norm - and only
                // widens the search; the test itself happens back in world space
                // where the shape is still a capsule.
                float squared = 0.0f;
                for (float value : instance.inverse)
                    squared += value * value;
                const float localRadius = radius * std::sqrt(squared);

                const auto toLocal = [&](const std::array<float, 3> &p) {
                    return apply(instance.inverse, {p[0] - instance.translation[0], p[1] - instance.translation[1],
                                                    p[2] - instance.translation[2]});
                };
                const std::array<float, 3> localLow = toLocal(low), localHigh = toLocal(high);
                const std::array<float, 6> localQuery{
                    std::min(localLow[0], localHigh[0]) - localRadius,
                    std::min(localLow[1], localHigh[1]) - localRadius,
                    std::min(localLow[2], localHigh[2]) - localRadius,
                    std::max(localLow[0], localHigh[0]) + localRadius,
                    std::max(localLow[1], localHigh[1]) + localRadius,
                    std::max(localLow[2], localHigh[2]) + localRadius};

                const MeshIndex &tree = *instance.index;
                std::size_t depth = 0;
                stack[depth++] = 0;
                while (depth != 0)
                {
                    const std::uint32_t which = stack[--depth];
                    const MeshIndex::Node &node = tree.nodes[which];
                    if (!overlapBounds(localQuery, node.bounds))
                        continue;
                    if (node.count == 0)
                    {
                        if (depth + 2 <= std::size(stack))
                        {
                            stack[depth++] = node.child;
                            stack[depth++] = which + 1;
                        }
                        continue;
                    }

                    for (std::uint32_t triangle = 0; triangle < node.count; ++triangle)
                    {
                        const float *t = &tree.triangles[(std::size_t(node.first) + triangle) * 9];
                        ++m_tested;
                        const std::array<float, 3> a = place(instance.world, {t[0], t[1], t[2]});
                        const std::array<float, 3> b = place(instance.world, {t[3], t[4], t[5]});
                        const std::array<float, 3> c = place(instance.world, {t[6], t[7], t[8]});

                        // The closest pair between the capsule's core and the
                        // triangle: the three edges against the core, and each
                        // end of the core against the face. The smallest of
                        // those is the distance, whichever feature owns it.
                        std::array<float, 3> onCore{}, onFace{};
                        float bestSquared = std::numeric_limits<float>::max();
                        const std::array<float, 3> *corners[3] = {&a, &b, &c};
                        for (int edge = 0; edge < 3; ++edge)
                        {
                            std::array<float, 3> c1{}, c2{};
                            closestBetweenSegments(low, high, *corners[edge], *corners[(edge + 1) % 3], c1, c2);
                            const float d[3] = {c1[0] - c2[0], c1[1] - c2[1], c1[2] - c2[2]};
                            const float distance = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                            if (distance < bestSquared)
                            {
                                bestSquared = distance;
                                onCore = c1;
                                onFace = c2;
                            }
                        }
                        for (const std::array<float, 3> &end : {low, high})
                        {
                            const std::array<float, 3> onIt = closestOnTriangle(end, a, b, c);
                            const float d[3] = {end[0] - onIt[0], end[1] - onIt[1], end[2] - onIt[2]};
                            const float distance = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                            if (distance < bestSquared)
                            {
                                bestSquared = distance;
                                onCore = end;
                                onFace = onIt;
                            }
                        }

                        if (bestSquared >= radius * radius)
                            continue;

                        Contact contact;
                        const float distance = std::sqrt(bestSquared);
                        if (distance > 1e-4f)
                        {
                            contact.normal = {(onCore[0] - onFace[0]) / distance,
                                              (onCore[1] - onFace[1]) / distance,
                                              (onCore[2] - onFace[2]) / distance};
                        }
                        else
                        {
                            // The core is on the surface, so which way out is
                            // not a difference of two points any more: the face
                            // decides.
                            const std::array<float, 3> u{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
                            const std::array<float, 3> v{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
                            std::array<float, 3> n{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                                                   u[0] * v[1] - u[1] * v[0]};
                            const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                            if (length < 1e-9f)
                                continue;
                            for (float &component : n)
                                component /= length;
                            contact.normal = n;
                        }
                        contact.depth = radius - distance;
                        out.push_back(contact);
                    }
                }
            }
        }

    return out.size() - before;
}

} // namespace physics
