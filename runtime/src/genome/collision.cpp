#include "collision.h"

#include "property_set.h"
#include "reader.h"

#include <algorithm>
#include <cstring>

namespace genome
{
namespace
{

// The cooked header, read straight out of the files rather than from any
// specification. Every blob in every sample carries the same shape:
//
//   +0   "NXS", a version byte, "MESH"
//   +16  the float 0.001, identical everywhere - the cooking tolerance
//   +28  the vertex count
//   +32  the triangle count
//   +36  the vertices, three floats each, then the indices
//
// The index width is not written down anywhere. It is 8, 16 or 32 bits and it
// varies within a single file: a flight of stairs has one part at 32 and its
// neighbours at 16. So each width is tried and the one that reads correctly is
// taken - see chooseIndexWidth for what "correctly" has to mean, because the
// obvious test is not enough.
constexpr std::size_t c_VertexCountAt = 28;
constexpr std::size_t c_HeaderBytes = 36;

const std::uint8_t *findTag(const std::uint8_t *from, const std::uint8_t *end, const char *tag)
{
    for (const std::uint8_t *at = from; at + 8 <= end; ++at)
        if (at[0] == 'N' && at[1] == 'X' && at[2] == 'S' && at[4] == tag[0] && at[5] == tag[1] &&
            at[6] == tag[2] && at[7] == tag[3])
            return at;
    return nullptr;
}

const std::uint8_t *findSignature(const std::uint8_t *from, const std::uint8_t *end)
{
    return findTag(from, end, "MESH");
}

std::uint32_t indexAt(const std::uint8_t *data, std::size_t at, unsigned width)
{
    if (width == 1)
        return data[at];
    if (width == 2)
    {
        std::uint16_t value = 0;
        std::memcpy(&value, data + at * 2, 2);
        return value;
    }
    std::uint32_t value = 0;
    std::memcpy(&value, data + at * 4, 4);
    return value;
}

// How many of the triangles are real, at a given index width. A triangle whose
// three corners are not three different vertices is degenerate - it has no area
// and collides with nothing - so a cooked mesh should have none.
std::size_t nonDegenerate(const std::uint8_t *indices, std::size_t triangles, unsigned width,
                          std::uint32_t vertices)
{
    std::size_t real = 0;
    for (std::size_t at = 0; at < triangles; ++at)
    {
        const std::uint32_t a = indexAt(indices, at * 3 + 0, width);
        const std::uint32_t b = indexAt(indices, at * 3 + 1, width);
        const std::uint32_t c = indexAt(indices, at * 3 + 2, width);
        if (a >= vertices || b >= vertices || c >= vertices)
            return 0;
        real += (a != b && b != c && a != c) ? 1 : 0;
    }
    return real;
}

// The width the indices are really in.
//
// The obvious test - every index lands inside the vertex list - is not enough,
// and finding that out was the whole value of testing it. Reading 16-bit
// indices as 8-bit leaves every second byte a zero: those zeroes are perfectly
// in range, so the mesh passes, and half its triangles come out as a corner
// repeated. The count of degenerate triangles is what tells the two apart, and
// it is unambiguous - a right reading has none and a wrong one has half.
//
// So every width that fits is scored, and the best is taken rather than the
// first.
unsigned chooseIndexWidth(const std::uint8_t *indices, std::size_t available, std::size_t triangles,
                          std::uint32_t vertices, std::size_t &realOut)
{
    unsigned best = 0;
    std::size_t bestReal = 0;
    for (unsigned width : {1u, 2u, 4u})
    {
        if (triangles * 3 * width > available)
            continue;
        const std::size_t real = nonDegenerate(indices, triangles, width, vertices);
        if (real > bestReal)
        {
            bestReal = real;
            best = width;
        }
    }
    realOut = bestReal;
    return best;
}

} // namespace

CollisionKind collisionKind(const std::vector<std::uint8_t> &bytes)
{
    const std::uint8_t *const begin = bytes.data();
    const std::uint8_t *const end = begin + bytes.size();
    if (findTag(begin, end, "MESH"))
        return CollisionKind::TriangleMesh;
    if (findTag(begin, end, "CVXM"))
        return CollisionKind::ConvexHull;
    return CollisionKind::Unknown;
}

std::size_t CollisionMesh::triangleCount() const
{
    std::size_t total = 0;
    for (const CollisionPart &part : parts)
        total += part.indices.size() / 3;
    return total;
}

std::size_t CollisionMesh::vertexCount() const
{
    std::size_t total = 0;
    for (const CollisionPart &part : parts)
        total += part.positions.size();
    return total;
}

bool loadCollisionMesh(const std::vector<std::uint8_t> &bytes, CollisionMesh &out, std::string *error)
{
    const auto fail = [error](const char *why) {
        if (error)
            *error = why;
        return false;
    };

    if (bytes.size() < 16)
        return fail("not a collision mesh: too short to hold anything");

    // The property set gives the class and the name. Its own contents are of no
    // use here beyond confirming what the file is, but confirming that is worth
    // the few lines: a file that is not eCResourceCollisionMesh_PS has no
    // business being read for triangles.
    Reader reader(bytes);
    StringTable strings;
    PropertySetHeader header;
    if (readPropertySetHeader(reader, strings, header) && !header.className.empty())
    {
        if (header.className != "eCResourceCollisionMesh_PS")
            return fail("not a collision mesh: the class is something else");
        out.name = header.objectName;
    }

    // The cooked blobs are found by their signature rather than by an offset.
    // The property set before them varies in length with the object's name, and
    // the number of blobs is not written anywhere that has been identified - a
    // stick has two, a flight of stairs three, most objects one.
    out.parts.clear();
    const std::uint8_t *const end = bytes.data() + bytes.size();
    const std::uint8_t *at = bytes.data();
    while (const std::uint8_t *blob = findSignature(at, end))
    {
        at = blob + 8;
        if (blob + c_HeaderBytes > end)
            break;

        std::uint32_t vertices = 0, triangles = 0;
        std::memcpy(&vertices, blob + c_VertexCountAt, 4);
        std::memcpy(&triangles, blob + c_VertexCountAt + 4, 4);
        if (vertices == 0 || triangles == 0)
            continue;

        const std::size_t positionBytes = std::size_t(vertices) * 12;
        if (std::size_t(end - blob) < c_HeaderBytes + positionBytes)
            continue;

        const std::uint8_t *indices = blob + c_HeaderBytes + positionBytes;
        std::size_t real = 0;
        const unsigned width =
            chooseIndexWidth(indices, std::size_t(end - indices), triangles, vertices, real);
        if (width == 0)
            return fail("a cooked mesh has no index width that keeps every index inside its vertices");
        if (real != triangles)
            return fail("a cooked mesh reads with degenerate triangles, so the reading is wrong");

        CollisionPart part;
        part.positions.resize(vertices);
        std::memcpy(part.positions.data(), blob + c_HeaderBytes, positionBytes);
        part.indices.resize(std::size_t(triangles) * 3);
        for (std::size_t which = 0; which < part.indices.size(); ++which)
            part.indices[which] = indexAt(indices, which, width);

        at = indices + std::size_t(triangles) * 3 * width;
        out.parts.push_back(std::move(part));
    }

    if (out.parts.empty())
        return fail(collisionKind(bytes) == CollisionKind::ConvexHull
                        ? "this is a convex hull, not a triangle mesh"
                        : "no cooked shape found in the file");
    return true;
}

} // namespace genome
