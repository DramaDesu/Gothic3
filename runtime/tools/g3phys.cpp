// Reads the game's collision meshes, and checks the reading against every one
// of them rather than against the handful that were looked at while working it
// out.
//
// With a name it dumps one; with none it walks the archive, which is how the
// format work gets checked - the same shape as g3lightmap.

#include "genome/collision.h"
#include "genome/mesh.h"
#include "genome/pak.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <vector>

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::puts("usage: g3phys <_compiledPhysic.pak> [_compiledMesh.pak]");
        std::puts("       g3phys <archive.pak> <name.xnvmsh> [out.obj]");
        return 2;
    }

    std::string error;
    const auto archive = genome::PakArchive::open(argv[1], &error);
    if (!archive)
    {
        std::cerr << "error: " << error << "\n";
        return 1;
    }

    const bool wantsOne = argc >= 3 && std::string(argv[2]).find(".xnvmsh") != std::string::npos;
    if (wantsOne)
    {
        genome::CollisionMesh mesh;
        if (!genome::loadCollisionMesh(archive->read(argv[2], &error), mesh, &error))
        {
            std::cerr << "error: " << error << "\n";
            return 1;
        }

        std::printf("%s: %zu parts, %zu vertices, %zu triangles\n", mesh.name.empty() ? argv[2] : mesh.name.c_str(),
                    mesh.parts.size(), mesh.vertexCount(), mesh.triangleCount());
        for (std::size_t which = 0; which < mesh.parts.size(); ++which)
        {
            const genome::CollisionPart &part = mesh.parts[which];
            std::array<float, 3> low{part.positions[0]}, high{part.positions[0]};
            for (const std::array<float, 3> &position : part.positions)
                for (int axis = 0; axis < 3; ++axis)
                {
                    low[axis] = std::min(low[axis], position[axis]);
                    high[axis] = std::max(high[axis], position[axis]);
                }
            std::printf("  part %zu: %6zu vertices, %6zu triangles, box %.3f %.3f %.3f to %.3f %.3f %.3f\n", which,
                        part.positions.size(), part.indices.size() / 3, low[0], low[1], low[2], high[0], high[1],
                        high[2]);
        }

        // A wavefront file, so the geometry can be looked at in something that
        // was not written here - which is the only way to find out whether a
        // reading that satisfies its own checks is the shape it claims to be.
        if (argc >= 4)
        {
            std::FILE *out = std::fopen(argv[3], "w");
            if (!out)
                return 1;
            std::size_t base = 1;
            for (const genome::CollisionPart &part : mesh.parts)
            {
                for (const std::array<float, 3> &position : part.positions)
                    std::fprintf(out, "v %g %g %g\n", position[0], position[1], position[2]);
                for (std::size_t at = 0; at + 2 < part.indices.size(); at += 3)
                    std::fprintf(out, "f %zu %zu %zu\n", base + part.indices[at], base + part.indices[at + 1],
                                 base + part.indices[at + 2]);
                base += part.positions.size();
            }
            std::fclose(out);
            std::printf("wrote %s\n", argv[3]);
        }
        return 0;
    }

    // The visual meshes, when a second archive is given, so the collision can
    // be checked against something that was not read by the same code.
    std::unique_ptr<genome::PakArchive> visual;
    if (argc >= 3 && !wantsOne)
    {
        visual = genome::PakArchive::open(argv[2], &error);
        if (!visual)
        {
            std::cerr << "error opening the mesh archive: " << error << "\n";
            return 1;
        }
    }
    std::size_t compared = 0, agreed = 0;
    std::vector<double> ratios;
    double worstGap = 0.0;
    std::string worstName;

    std::size_t parsed = 0, failed = 0, parts = 0, vertices = 0, triangles = 0;
    std::size_t hulls = 0;
    std::size_t biggest = 0;
    std::string biggestName;
    std::vector<std::size_t> perFile;
    std::map<std::string, std::size_t> reasons;
    std::map<std::string, std::size_t> carried;
    std::map<std::string, std::string> firstOfKind;

    for (const genome::PakEntry &entry : archive->entries())
    {
        if (entry.deleted || entry.path.find(".xnvmsh") == std::string::npos)
            continue;

        const std::vector<std::uint8_t> raw = archive->read(entry, &error);
        genome::CollisionMesh mesh;
        if (!genome::loadCollisionMesh(raw, mesh, &error))
        {
            ++failed;
            if (reasons.size() < 12)
                ++reasons[error];

            // Which kind it was, so that a failure says what it failed on.
            const genome::CollisionKind kind = genome::collisionKind(raw);
            const char *carries = kind == genome::CollisionKind::ConvexHull ? "a convex hull"
                                  : kind == genome::CollisionKind::TriangleMesh
                                      ? "a triangle mesh that would not read"
                                      : "no cooked shape at all";
            ++carried[carries];
            if (firstOfKind.find(carries) == firstOfKind.end())
                firstOfKind[carries] = entry.path;
            continue;
        }
        ++parsed;
        // A file is one shape or the other, so its first part says which.
        hulls += mesh.parts.front().convex ? 1 : 0;
        parts += mesh.parts.size();
        vertices += mesh.vertexCount();
        const std::size_t count = mesh.triangleCount();
        triangles += count;
        perFile.push_back(count);

        // Against the visual mesh of the same name. A collision hull is built
        // around the object it collides for, so the two boxes should agree -
        // not exactly, since collision is simplified, but closely.
        if (visual)
        {
            std::string meshName = entry.path;
            const std::size_t slash = meshName.find_last_of('/');
            if (slash != std::string::npos)
                meshName = meshName.substr(slash + 1);
            const std::size_t dot = meshName.find_last_of('.');
            if (dot != std::string::npos)
                meshName = meshName.substr(0, dot) + ".xcmsh";

            std::string ignored;
            genome::Mesh drawn;
            if (genome::loadMesh(visual->read(meshName, &ignored), drawn, &ignored))
            {
                std::array<float, 3> lowA{}, highA{}, lowB{}, highB{};
                bool haveA = false, haveB = false;
                // Times a hundred, because the collision is in metres and
                // everything drawn is in centimetres. That is not a guess: a
                // grindstone wheel is 0.259 across in collision and 25.9 drawn,
                // with fourteen triangles on each side and four parts against
                // four elements. PhysX works in SI units and the cooking kept
                // them; the renderer never had to care.
                constexpr float c_MetresToWorld = 100.0f;
                for (const genome::CollisionPart &part : mesh.parts)
                    for (const std::array<float, 3> &raw : part.positions)
                        for (int axis = 0; axis < 3; ++axis)
                        {
                            const float position = raw[axis] * c_MetresToWorld;
                            lowA[axis] = haveA ? std::min(lowA[axis], position) : position;
                            highA[axis] = haveA ? std::max(highA[axis], position) : position;
                            haveA = true;
                        }
                for (const genome::MeshElement &element : drawn.elements)
                    for (const std::array<float, 3> &position : element.positions)
                        for (int axis = 0; axis < 3; ++axis)
                        {
                            lowB[axis] = haveB ? std::min(lowB[axis], position[axis]) : position[axis];
                            highB[axis] = haveB ? std::max(highB[axis], position[axis]) : position[axis];
                            haveB = true;
                        }
                if (haveA && haveB)
                {
                    ++compared;
                    double gap = 0.0, span = 1e-6;
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        gap = std::max(gap, double(std::abs(lowA[axis] - lowB[axis])));
                        gap = std::max(gap, double(std::abs(highA[axis] - highB[axis])));
                        span = std::max(span, double(highB[axis] - lowB[axis]));
                    }
                    // The ratio of the two spans, which is what says whether
                    // the disagreement is a wrong reading or a different unit.
                    double spanA = 1e-6;
                    for (int axis = 0; axis < 3; ++axis)
                        spanA = std::max(spanA, double(highA[axis] - lowA[axis]));
                    ratios.push_back(span / spanA);

                    const double relative = gap / span;
                    agreed += relative < 0.25 ? 1 : 0;
                    if (relative > worstGap)
                    {
                        worstGap = relative;
                        worstName = entry.path;
                    }
                }
            }
        }
        if (count > biggest)
        {
            biggest = count;
            biggestName = entry.path;
        }
    }

    std::printf("%zu triangle meshes, %zu convex hulls, %zu that would not read\n", parsed - hulls, hulls,
                failed);
    std::printf("%zu parts, %zu vertices, %zu triangles in all\n", parts, vertices, triangles);
    if (!perFile.empty())
    {
        std::sort(perFile.begin(), perFile.end());
        std::printf("triangles per object: %zu at the median, %zu at the 90th, %zu at most (%s)\n",
                    perFile[perFile.size() / 2], perFile[perFile.size() * 9 / 10], biggest, biggestName.c_str());
    }
    if (!ratios.empty())
    {
        std::sort(ratios.begin(), ratios.end());
        std::printf("the drawn mesh is %.4g times the size of the collision at the median, %.4g at the 10th, "
                    "%.4g at the 90th\n",
                    ratios[ratios.size() / 2], ratios[ratios.size() / 10], ratios[ratios.size() * 9 / 10]);
    }
    if (compared != 0)
        std::printf("compared %zu against the mesh they collide for: %zu agree within a quarter of the object, "
                    "worst is %.0f%% off (%s)\n",
                    compared, agreed, 100.0 * worstGap, worstName.c_str());
    for (const auto &[what, count] : carried)
        std::printf("  %-32s %5zu   e.g. %s\n", what.c_str(), count, firstOfKind[what].c_str());
    for (const auto &[reason, count] : reasons)
        std::printf("  %5zu  %s\n", count, reason.c_str());
    return failed == 0 ? 0 : 1;
}
