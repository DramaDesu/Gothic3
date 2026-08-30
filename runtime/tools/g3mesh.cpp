// Loads meshes out of an archive. With a name it dumps one mesh; without, it
// parses every mesh in the archive, which is the real test of the format work.

#include "genome/mesh.h"
#include "genome/pak.h"

#include <cstdio>
#include <iostream>
#include <map>

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::puts("usage: g3mesh <archive.pak> [mesh name]");
        return 2;
    }

    std::string error;
    const auto archive = genome::PakArchive::open(argv[1], &error);
    if (!archive)
    {
        std::cerr << "error: " << error << "\n";
        return 1;
    }

    if (argc >= 3)
    {
        genome::Mesh mesh;
        const std::vector<std::uint8_t> bytes = archive->read(argv[2], &error);
        if (bytes.empty() || !genome::loadMesh(bytes, mesh, &error))
        {
            std::cerr << "error: " << error << "\n";
            return 1;
        }

        std::printf("%s: %zu elements, %zu vertices, %zu triangles\n", mesh.name.c_str(), mesh.elements.size(),
                    mesh.vertexCount(), mesh.triangleCount());
        std::printf("bounds  %.1f %.1f %.1f  ..  %.1f %.1f %.1f\n", mesh.boundsMin[0], mesh.boundsMin[1],
                    mesh.boundsMin[2], mesh.boundsMax[0], mesh.boundsMax[1], mesh.boundsMax[2]);
        for (std::size_t index = 0; index < mesh.elements.size(); ++index)
        {
            const genome::MeshElement &element = mesh.elements[index];
            std::printf("  [%zu] %-44s %6zu verts %6zu tris  %s%s%s\n", index, element.materialName.c_str(),
                        element.positions.size(), element.triangleCount(), element.normals.empty() ? "" : "N",
                        element.texCoords.empty() ? "" : "UV", element.tangents.empty() ? "" : "T");
        }
        return 0;
    }

    std::size_t parsed = 0, failed = 0, vertices = 0, triangles = 0;
    std::map<std::string, std::size_t> reasons;
    for (const genome::PakEntry &entry : archive->entries())
    {
        if (entry.deleted || entry.path.find(".xcmsh") == std::string::npos)
            continue;

        genome::Mesh mesh;
        const std::vector<std::uint8_t> bytes = archive->read(entry, &error);
        if (bytes.empty() || !genome::loadMesh(bytes, mesh, &error))
        {
            ++failed;
            if (reasons.size() < 20)
                ++reasons[error];
            continue;
        }
        ++parsed;
        vertices += mesh.vertexCount();
        triangles += mesh.triangleCount();
    }

    std::printf("parsed %zu meshes, %zu failed\n", parsed, failed);
    std::printf("%zu vertices, %zu triangles in total\n", vertices, triangles);
    for (const auto &[reason, count] : reasons)
        std::printf("  %5zu  %s\n", count, reason.c_str());
    return failed == 0 ? 0 : 1;
}
