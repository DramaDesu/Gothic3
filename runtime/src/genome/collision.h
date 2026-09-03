#pragma once

// The collision geometry, which the game ships separately from what it draws.
//
// _compiledPhysic.pak holds 4501 of these and the mesh archive another 609: one
// .xnvmsh per object, median 9691 bytes, and far simpler than the mesh beside
// it - a chest is 9 vertices and 14 triangles where the visual one is hundreds.
// That separation is why walking the world is a smaller problem than drawing
// it.
//
// A file is an ordinary Genome property set of class eCResourceCollisionMesh_PS
// wrapping one or more cooked NovodeX meshes, which is what PhysX 2.x used when
// the game shipped in 2006. Nothing modern reads that format - PhysX 3.0 broke
// compatibility with it and never looked back - so the triangles are recovered
// here, and whatever collides with them afterwards is a separate question.
//
// Only the geometry is taken. Each blob also carries an OPCODE tree, which was
// its acceleration structure; we build our own or hand the triangles to
// something that does, so it is skipped.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace genome
{

// One cooked mesh. A file holds several when the object was authored in pieces:
// a stick is two, a flight of stairs is three.
struct CollisionPart
{
    std::vector<std::array<float, 3>> positions;
    std::vector<std::uint32_t> indices; // three per triangle, into positions
};

struct CollisionMesh
{
    std::string name;
    std::vector<CollisionPart> parts;

    std::size_t triangleCount() const;
    std::size_t vertexCount() const;
};

// What a .xnvmsh holds. Two of the three are real answers rather than errors:
// of the 6735 files, 3940 are triangle meshes and 2795 are convex hulls, whose
// names end in _cv - helmets and armour, the things thrown about rather than
// stood on.
enum class CollisionKind
{
    TriangleMesh,
    ConvexHull,
    Unknown,
};

// Says which, without reading the geometry. Cheap: it looks for the signature.
CollisionKind collisionKind(const std::vector<std::uint8_t> &bytes);

// Reads a .xnvmsh. False and `error` when the container is not what we expect,
// which includes every way the geometry could come out wrong: an index outside
// its own vertices, or triangles that collapse to a line. Those checks are the
// point - the counts alone will read plausibly from the wrong offset.
bool loadCollisionMesh(const std::vector<std::uint8_t> &bytes, CollisionMesh &out, std::string *error = nullptr);

} // namespace genome
