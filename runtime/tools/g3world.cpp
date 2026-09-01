// Draws the landscape. The low-poly tiles already carry world coordinates, so
// this needs no placement data: load them all and fly around.
//
//   g3world <_compiledMesh.pak> [name filter]
//
// Hold the right mouse button to look, WASD to move, Q/E to drop and rise,
// Shift to go faster, Ctrl to creep, O to toggle occlusion culling.

#include "genome/image.h"
#include "genome/material.h"
#include "genome/mesh.h"
#include "genome/pak.h"
#include "genome/spt.h"
#include "genome/tree.h"
#include "genome/lightmap.h"
#include "genome/world.h"
#include "render/window.h"
#include "render/profile.h"
#include "render/tree_atlas.h"
#include "render/world_renderer.h"

// windows.h is here only for the virtual-key codes, and its min/max macros
// would shadow the standard ones.
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <set>

namespace
{

std::array<float, 16> lookAt(const std::array<float, 3> &eye, const std::array<float, 3> &target)
{
    std::array<float, 3> forward{target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    const float length = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    for (float &value : forward)
        value /= length;

    const std::array<float, 3> worldUp{0.0f, 1.0f, 0.0f};
    std::array<float, 3> right{forward[1] * worldUp[2] - forward[2] * worldUp[1],
                               forward[2] * worldUp[0] - forward[0] * worldUp[2],
                               forward[0] * worldUp[1] - forward[1] * worldUp[0]};
    const float rightLength = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    for (float &value : right)
        value /= rightLength;

    const std::array<float, 3> up{right[1] * forward[2] - right[2] * forward[1],
                                  right[2] * forward[0] - right[0] * forward[2],
                                  right[0] * forward[1] - right[1] * forward[0]};

    return {right[0],
            up[0],
            -forward[0],
            0.0f,
            right[1],
            up[1],
            -forward[1],
            0.0f,
            right[2],
            up[2],
            -forward[2],
            0.0f,
            -(right[0] * eye[0] + right[1] * eye[1] + right[2] * eye[2]),
            -(up[0] * eye[0] + up[1] * eye[1] + up[2] * eye[2]),
            (forward[0] * eye[0] + forward[1] * eye[1] + forward[2] * eye[2]),
            1.0f};
}

std::array<float, 16> perspective(float fovRadians, float aspect, float nearPlane, float farPlane)
{
    const float f = 1.0f / std::tan(fovRadians * 0.5f);
    std::array<float, 16> m{};
    m[0] = f / aspect;
    m[5] = -f;
    m[10] = farPlane / (nearPlane - farPlane);
    m[11] = -1.0f;
    m[14] = (nearPlane * farPlane) / (nearPlane - farPlane);
    return m;
}

std::array<float, 16> multiply(const std::array<float, 16> &a, const std::array<float, 16> &b)
{
    std::array<float, 16> out{};
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
                sum += a[k * 4 + row] * b[column * 4 + k];
            out[column * 4 + row] = sum;
        }
    return out;
}

} // namespace

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    if (argc < 2)
    {
        std::puts("usage: g3world <_compiledMesh.pak> [mesh filter] [--sectors <Projects_compiled.pak> [sector "
              "filter]] [--shot <out.ppm>]");
        return 2;
    }

    const bool hasFilter = argc > 2 && argv[2][0] != 45;
    const std::string filter = hasFilter ? argv[2] : "g3_world_lowpoly_landscape_01/";

    // The mesh archive sits in the game's Data folder, so its siblings are found
    // beside it rather than asked for separately.
    std::string dataDirectory = argv[1];
    const std::size_t lastSlash = dataDirectory.find_last_of("/\\");
    dataDirectory = lastSlash == std::string::npos ? std::string(".") : dataDirectory.substr(0, lastSlash);

    std::string error;
    const auto archive = genome::PakArchive::open(argv[1], &error);
    if (!archive)
    {
        std::cerr << "error: " << error << "\n";
        return 1;
    }

    std::vector<std::unique_ptr<genome::Image>> images;

    // A batch is one mesh and every transform it is placed with. Landscape
    // tiles are already in world space, so they get a single identity instance.
    std::vector<std::unique_ptr<genome::Mesh>> ownedMeshes;
    std::map<std::string, std::size_t> batchOf;
    std::vector<render::MeshInstances> batches;
    // Which batch holds the full-detail mesh of each tree variant, in the order
    // they were grown - the billboard atlas is baked from exactly these.
    std::vector<std::size_t> treeFullDetail;
    std::vector<genome::PointLight> worldLights;
    // Every instance's baked vertex lighting, end to end. The shader indexes it
    // by the base each instance carries, which is how per-instance lighting
    // survives sharing one vertex buffer between instances.
    std::vector<std::uint32_t> lightmapColours;
    std::unique_ptr<genome::PakArchive> lightmapArchive;
    std::size_t lightmapsFound = 0, lightmapsMissing = 0;
    static const genome::WorldMatrix c_Identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    const bool wantLandscape = filter != "none";
    std::size_t failed = 0;
    if (wantLandscape)
    {
        for (const genome::PakEntry &entry : archive->entries())
        {
            if (entry.deleted || entry.path.find(filter) == std::string::npos)
                continue;
            if (entry.path.size() < 6 || entry.path.compare(entry.path.size() - 6, 6, ".xcmsh") != 0)
                continue;

            auto mesh = std::make_unique<genome::Mesh>();
            if (!genome::loadMesh(archive->read(entry, &error), *mesh, &error))
            {
                ++failed;
                continue;
            }

            render::MeshInstances batch;
            batch.mesh = mesh.get();
            batch.transforms.push_back(c_Identity);
            batches.push_back(std::move(batch));
            ownedMeshes.push_back(std::move(mesh));
        }
    }

    int sectorArgument = 0;
    const char *shotPath = nullptr;
    // Everything so far has been counted in objects. This counts milliseconds.
    int benchFrames = 0;
    int cameraArgument = 0;
    int treeArgument = 0;
    bool validation = false;
    // How many different trees are grown per definition before they repeat.
    constexpr std::uint32_t c_TreeVariants = 3;
    // Where a tree drops to its thinned form, in world units - a metre is a
    // hundred, so this is sixty metres.
    float treeLodDistance = 6000.0f;
    // And where it becomes a single quad. Four hundred metres.
    float treeBillboardDistance = 40000.0f;
    for (int index = 2; index + 1 < argc; ++index)
    {
        if (std::string(argv[index]) == "--sectors")
            sectorArgument = index + 1;
        if (std::string(argv[index]) == "--shot")
            shotPath = argv[index + 1];
        if (std::string(argv[index]) == "--lod" && index + 1 < argc)
            treeLodDistance = float(std::atof(argv[index + 1]));
        if (std::string(argv[index]) == "--billboard" && index + 1 < argc)
            treeBillboardDistance = float(std::atof(argv[index + 1]));
        if (std::string(argv[index]) == "--bench" && index + 1 < argc)
            benchFrames = std::atoi(argv[index + 1]);
        if (std::string(argv[index]) == "--camera" && index + 5 < argc)
            cameraArgument = index + 1;
        if (std::string(argv[index]) == "--tree" && index + 1 < argc)
            treeArgument = index + 1;
        if (std::string(argv[index]) == "--validate")
            validation = true;
    }

    const bool showOneTree = treeArgument != 0 && treeArgument + 1 < argc &&
                             std::string(argv[treeArgument + 1]).find(".spt") != std::string::npos;
    if (showOneTree)
    {
        const auto trees = genome::PakArchive::open(argv[treeArgument], nullptr);
        genome::SpeedTree definition;
        if (!trees || !genome::loadSpeedTree(trees->read(argv[treeArgument + 1], &error), definition, &error))
            std::printf("warning: could not read %s: %s\n", argv[treeArgument + 1], error.c_str());
        else
        {
            // A row of them, so the variance between instances is visible rather
            // than asserted.
            for (std::uint32_t index = 0; index < 5; ++index)
            {
                auto mesh = std::make_unique<genome::Mesh>();
                if (!genome::growTree(definition, definition.seed + index * 7919u, genome::TreeGrowth{}, *mesh))
                    continue;

                render::MeshInstances batch;
                batch.mesh = mesh.get();
                batch.occludes = false;
                genome::WorldMatrix world{};
                world[0] = world[5] = world[10] = world[15] = 1.0f;
                world[12] = float(index) * (mesh->boundsMax[0] - mesh->boundsMin[0] + 200.0f);
                batch.transforms.push_back(world);
                batches.push_back(std::move(batch));
                ownedMeshes.push_back(std::move(mesh));
            }
            std::printf("grew %zu trees from %s\n", batches.size(), argv[treeArgument + 1]);
        }
    }

    lightmapArchive = genome::PakArchive::open(dataDirectory + "/Lightmaps.pak", nullptr);

    // Sectors name their trees by definition; growing one mesh per definition
    // and instancing it is the only way 57315 of them fit, and it is what the
    // game itself did - it batched every tree of a species from one buffer.
    std::unique_ptr<genome::PakArchive> treeArchive;
    std::map<std::string, std::string> treePathOf;
    if (treeArgument != 0)
    {
        treeArchive = genome::PakArchive::open(argv[treeArgument], nullptr);
        if (treeArchive)
            for (const genome::PakEntry &entry : treeArchive->entries())
            {
                if (entry.deleted)
                    continue;
                const std::size_t slash = entry.path.find_last_of('/');
                std::string name = slash == std::string::npos ? entry.path : entry.path.substr(slash + 1);
                treePathOf.emplace(name, entry.path);
            }
    }

    if (sectorArgument != 0)
    {
        const auto world = genome::PakArchive::open(argv[sectorArgument], nullptr);
        if (!world)
            std::puts("warning: could not open the world archive");
        else
        {
            const std::string sectorFilter =
                sectorArgument + 1 < argc ? argv[sectorArgument + 1] : "_cstat.node";
            std::size_t placed = 0, sectors = 0, missing = 0, grass = 0, planted = 0, missingTrees = 0;
            std::map<std::string, std::size_t> treeBatchOf;

            for (const genome::PakEntry &entry : world->entries())
            {
                if (entry.deleted || entry.path.find(sectorFilter) == std::string::npos)
                    continue;

                genome::WorldLayer layer;
                std::string ignored;
                if (!genome::loadWorldNode(world->read(entry, &ignored), layer, &ignored))
                    continue;
                ++sectors;

                for (const genome::Placement &placement : layer.placements)
                {
                    if (placement.meshName.empty())
                        continue;

                    // The baked lighting of this instance, found by the mesh it
                    // places and its own identifier - which is exactly how the
                    // archive names its files.
                    std::int32_t lightmapBase = -1;
                    if (lightmapArchive && !placement.guid.empty())
                    {
                        std::string name = placement.meshName;
                        const std::size_t dot = name.find_last_of('.');
                        if (dot != std::string::npos)
                            name = name.substr(0, dot);
                        name += "_{" + placement.guid + "}.xlmp";

                        std::string why;
                        genome::Lightmap map;
                        const std::vector<std::uint8_t> bytes = lightmapArchive->read(name, &why);
                        if (!bytes.empty() && genome::loadLightmap(bytes, map, &why) && !map.elements.empty() &&
                            !map.elements.front().colours.empty())
                        {
                            lightmapBase = std::int32_t(lightmapColours.size());
                            for (const genome::LightmapElement &element : map.elements)
                                lightmapColours.insert(lightmapColours.end(), element.colours.begin(),
                                                       element.colours.end());
                            ++lightmapsFound;
                        }
                        else
                            ++lightmapsMissing;
                    }

                    const auto known = batchOf.find(placement.meshName);
                    if (known != batchOf.end())
                    {
                        if (known->second != std::size_t(-1))
                        {
                            batches[known->second].lightmapBase.push_back(lightmapBase);
                            batches[known->second].transforms.push_back(placement.world);
                            batches[known->second].bounds.push_back(
                                {placement.boundsMin[0], placement.boundsMin[1], placement.boundsMin[2],
                                 placement.boundsMax[0], placement.boundsMax[1], placement.boundsMax[2]});
                            ++placed;
                        }
                        continue;
                    }

                    auto mesh = std::make_unique<genome::Mesh>();
                    if (!genome::loadMesh(archive->read(placement.meshName, &ignored), *mesh, &ignored))
                    {
                        ++missing;
                        batchOf.emplace(placement.meshName, std::size_t(-1));
                        continue;
                    }

                    render::MeshInstances batch;
                    batch.mesh = mesh.get();
                    batch.lightmapBase.push_back(lightmapBase);
                    batch.transforms.push_back(placement.world);
                    batch.bounds.push_back({placement.boundsMin[0], placement.boundsMin[1], placement.boundsMin[2],
                                            placement.boundsMax[0], placement.boundsMax[1], placement.boundsMax[2]});
                    batchOf.emplace(placement.meshName, batches.size());
                    batches.push_back(std::move(batch));
                    ownedMeshes.push_back(std::move(mesh));
                    ++placed;
                }
                worldLights.insert(worldLights.end(), layer.lights.begin(), layer.lights.end());

                for (const genome::TreePlacement &tree : layer.trees)
                {
                    if (!treeArchive)
                        break;

                    // A handful of seeds per definition, so a wood is not one
                    // tree repeated, and the mesh for each is grown once.
                    const std::uint32_t variant = std::uint32_t(planted) % c_TreeVariants;
                    std::string key = tree.resource;
                    for (char &c : key)
                        c = char(std::tolower(static_cast<unsigned char>(c)));
                    key += char('0' + variant);

                    auto known = treeBatchOf.find(key);
                    if (known == treeBatchOf.end())
                    {
                        std::string path;
                        const auto found = treePathOf.find(key.substr(0, key.size() - 1));
                        if (found != treePathOf.end())
                            path = found->second;

                        std::size_t slot = std::size_t(-1);
                        genome::SpeedTree definition;
                        std::string why;
                        if (!path.empty() &&
                            genome::loadSpeedTree(treeArchive->read(path, &why), definition, &why))
                        {
                            // Two of each: the full tree for close up and a
                            // thinned one past the switch distance. A tree is
                            // four thousand triangles and there are 29138 of
                            // them in view at once, which is where the frame
                            // goes; a distant one covering a few pixels must not
                            // cost the same as one filling the screen.
                            const std::uint32_t seed = definition.seed + variant * 7919u;
                            for (int level = 0; level < 2; ++level)
                            {
                                genome::TreeGrowth growth;
                                growth.detail = level == 0 ? 1.0f : 0.15f;

                                auto mesh = std::make_unique<genome::Mesh>();
                                if (!genome::growTree(definition, seed, growth, *mesh))
                                    continue;

                                render::MeshInstances batch;
                                batch.mesh = mesh.get();
                                batch.occludes = false;
                                batch.lodNear = level == 0 ? 0.0f : treeLodDistance;
                                batch.lodFar = level == 0 ? treeLodDistance : 0.0f;
                                if (level == 0)
                                {
                                    slot = batches.size();
                                    treeFullDetail.push_back(slot);
                                }
                                batches.push_back(std::move(batch));
                                ownedMeshes.push_back(std::move(mesh));
                            }
                        }
                        else
                            ++missingTrees;
                        known = treeBatchOf.emplace(key, slot).first;
                    }

                    if (known->second == std::size_t(-1))
                        continue;

                    // The sector already knows how big the tree ends up, so its
                    // own bounds decide visibility rather than the grown mesh.
                    // Both detail levels hold every instance; the distance band
                    // decides which one draws it, so nothing is placed twice.
                    const std::array<float, 6> box{tree.boundsMin[0], tree.boundsMin[1], tree.boundsMin[2],
                                                   tree.boundsMax[0], tree.boundsMax[1], tree.boundsMax[2]};
                    for (std::size_t level = 0; level < 2; ++level)
                    {
                        const std::size_t at = known->second + level;
                        if (at >= batches.size())
                            break;
                        batches[at].transforms.push_back(tree.world);
                        batches[at].bounds.push_back(box);
                    }
                    ++planted;
                }

                // Grass is scattered rather than placed: the sector holds one mesh
                // per plant kind, and a grid of instances referring to them.
                const std::size_t firstPlantBatch = batches.size();
                for (const genome::VegetationMesh &plant : layer.vegetationMeshes)
                {
                    auto mesh = std::make_unique<genome::Mesh>();
                    genome::MeshElement element;
                    element.positions = plant.positions;
                    element.normals = plant.normals;
                    element.texCoords = plant.texCoords;
                    element.indices = plant.indices;
                    element.materialName = plant.texture;
                    mesh->elements.push_back(std::move(element));

                    render::MeshInstances batch;
                    batch.mesh = mesh.get();
                    batch.occludes = false;
                    batches.push_back(std::move(batch));
                    ownedMeshes.push_back(std::move(mesh));
                }

                for (const genome::VegetationInstance &plant : layer.vegetation)
                {
                    if (plant.mesh >= layer.vegetationMeshes.size())
                        continue;
                    render::MeshInstances &batch = batches[firstPlantBatch + plant.mesh];
                    batch.transforms.push_back(plant.world);
                    batch.bounds.push_back({plant.boundsMin[0], plant.boundsMin[1], plant.boundsMin[2],
                                            plant.boundsMax[0], plant.boundsMax[1], plant.boundsMax[2]});
                    ++grass;
                }
            }
            std::printf("%zu sectors, %zu objects placed, %zu meshes missing, %zu plants\n", sectors, placed,
                        missing, grass);
            if (planted != 0 || missingTrees != 0)
                std::printf("%zu trees planted from %zu grown meshes, %zu definitions missing\n", planted,
                            treeBatchOf.size(), missingTrees);
        }
    }

    if (batches.empty())
    {
        std::puts("nothing to draw");
        return 1;
    }

    std::size_t vertices = 0, triangles = 0, instances = 0;
    for (const render::MeshInstances &batch : batches)
    {
        vertices += batch.mesh->vertexCount();
        triangles += batch.mesh->triangleCount();
        instances += batch.transforms.size();
    }
    std::printf("%zu distinct meshes (%zu failed), %zu instances, %zu unique vertices, %zu unique triangles\n",
                batches.size(), failed, instances, vertices, triangles);

    // Every mesh element names a material, and those resolve to textures that
    // are shared across the meshes using them.
    {
        const auto materials = genome::PakArchive::open(dataDirectory + "/_compiledMaterial.pak", nullptr);
        const auto imageArchive = genome::PakArchive::open(dataDirectory + "/_compiledImage.pak", nullptr);

        std::set<std::string> imageNames;
        if (imageArchive)
        {
            for (const genome::PakEntry &entry : imageArchive->entries())
            {
                if (entry.deleted || entry.path.size() < 6 ||
                    entry.path.compare(entry.path.size() - 5, 5, ".ximg") != 0)
                    continue;
                const std::size_t slash = entry.path.find_last_of('/');
                const std::string leaf = slash == std::string::npos ? entry.path : entry.path.substr(slash + 1);
                imageNames.insert(leaf.substr(0, leaf.size() - 5));
            }
        }
        const auto exists = [&](const std::string &name) { return imageNames.count(name) != 0; };

        // Water has no diffuse slot at all - its look comes from a dedicated
        // shader - so without a stand-in every river renders as white.
        auto water = std::make_unique<genome::Image>();
        water->width = 1;
        water->height = 1;
        water->faceCount = 1;
        water->format = genome::ImageFormat::A8R8G8B8;
        water->data = {150, 105, 45, 255}; // BGRA
        water->levels.push_back({1, 1, 0, 4});
        water->faceStride = 4;
        const genome::Image *waterImage = water.get();
        images.push_back(std::move(water));

        std::map<std::string, const genome::Image *> cache;
        // Which materials throw away transparent pixels, remembered per name so
        // the answer survives the texture cache.
        std::map<std::string, bool> masked;
        for (render::MeshInstances &batch : batches)
        {
            for (const genome::MeshElement &element : batch.mesh->elements)
            {
                const genome::Image *loaded = nullptr;
                const auto cached = cache.find(element.materialName);
                if (cached != cache.end())
                    loaded = cached->second;
                else if (imageArchive && element.materialName.size() > 4 &&
                         (element.materialName.compare(element.materialName.size() - 4, 4, ".dds") == 0 ||
                          element.materialName.compare(element.materialName.size() - 4, 4, ".tga") == 0))
                {
                    // Grass and trees name their textures outright, in the form
                    // they were authored under rather than through a material.
                    std::string base = element.materialName.substr(0, element.materialName.size() - 4);
                    for (char &c : base)
                        c = char(std::tolower(static_cast<unsigned char>(c)));

                    std::string ignored;
                    auto image = std::make_unique<genome::Image>();
                    if (exists(base) && genome::loadImage(imageArchive->read(base + ".ximg", &ignored), *image,
                                                          &ignored))
                    {
                        loaded = image.get();
                        images.push_back(std::move(image));
                    }
                    cache.emplace(element.materialName, loaded);
                    // Our own foliage - grass patches, leaves and fronds - is
                    // quads whose texture is mostly empty.
                    masked.emplace(element.materialName, true);
                }
                else if (materials && imageArchive && !element.materialName.empty())
                {
                    genome::Material material;
                    std::string ignored;
                    if (genome::loadMaterial(materials->read(element.materialName, &ignored), material, &ignored))
                    {
                        // The game says which surfaces mask; taking every alpha
                        // channel at face value punches holes through stone.
                        masked.emplace(element.materialName, material.blendMode == genome::BlendMode::Masked);
                        if (material.kind == genome::ShaderKind::Water)
                            loaded = waterImage;
                        else if (const genome::Sampler *sampler = material.texture(genome::Slot::Diffuse))
                        {
                            const genome::TextureResolution resolved = genome::resolveTexture(*sampler, 0, exists);
                            if (!resolved.fileName.empty())
                            {
                                auto image = std::make_unique<genome::Image>();
                                if (genome::loadImage(imageArchive->read(resolved.fileName, &ignored), *image,
                                                      &ignored))
                                {
                                    loaded = image.get();
                                    images.push_back(std::move(image));
                                }
                            }
                        }
                    }
                    cache.emplace(element.materialName, loaded);
                }
                batch.textures.push_back(loaded);
                const auto isMasked = masked.find(element.materialName);
                batch.alphaTested.push_back(isMasked != masked.end() && isMasked->second ? 1 : 0);
            }
        }
        std::size_t untextured = 0;
        for (const render::MeshInstances &batch : batches)
            for (const genome::Image *texture : batch.textures)
                untextured += texture == nullptr ? 1 : 0;

        std::printf("%zu distinct textures, %zu mesh elements left untextured\n", images.size(), untextured);
        if (untextured != 0)
        {
            // Name a few so the gap is diagnosable rather than just white.
            std::size_t named = 0;
            for (const render::MeshInstances &batch : batches)
            {
                for (std::size_t element = 0; element < batch.textures.size() && named < 6; ++element)
                {
                    if (batch.textures[element] != nullptr)
                        continue;
                    const std::string &material = batch.mesh->elements[element].materialName;
                    std::printf("    no texture for %s\n", material.empty() ? "(no material named)" : material.c_str());
                    ++named;
                }
                if (named >= 6)
                    break;
            }
        }
    }

    render::Window window("Genome runtime - world", 1280, 720);
    render::Device device;
    if (!device.create(window, &error, validation))
    {
        std::cerr << "vulkan: " << error << "\n";
        return 1;
    }

    // Third detail level: a billboard. The trees we grew are drawn once each
    // into an atlas of our own, and past the far distance an instance becomes a
    // single quad sampling its own cell. The game shipped billboards but left
    // the field naming each tree's cell unset in 78 of its 98 definitions, so
    // baking our own is both easier and more correct.
    render::TreeAtlas treeAtlas;
    genome::Image treeAtlasImage;
    if (!treeFullDetail.empty())
    {
        std::vector<render::MeshInstances> bakeBatches;
        std::vector<std::size_t> bakeOrder;
        for (std::size_t slot : treeFullDetail)
        {
            render::MeshInstances one;
            one.mesh = batches[slot].mesh;
            one.textures = batches[slot].textures;
            one.alphaTested = batches[slot].alphaTested;
            one.transforms.push_back(c_Identity);
            // The mesh sits at the origin, so its own bounds fit the camera.
            std::array<float, 6> box{1e9f, 1e9f, 1e9f, -1e9f, -1e9f, -1e9f};
            for (const genome::MeshElement &element : one.mesh->elements)
                for (const std::array<float, 3> &position : element.positions)
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        box[axis] = std::min(box[axis], position[axis]);
                        box[axis + 3] = std::max(box[axis + 3], position[axis]);
                    }
            one.bounds.push_back(box);
            bakeOrder.push_back(bakeBatches.size());
            bakeBatches.push_back(std::move(one));
        }

        render::WorldRenderer baker;
        if (baker.create(device, bakeBatches, &error) &&
            render::bakeTreeAtlas(device, baker, bakeOrder, 256, treeAtlas, &error) &&
            render::readTreeAtlas(device, treeAtlas, treeAtlasImage, &error))
        {
            std::printf("baked %zu tree billboards into a %ux%u atlas\n", treeAtlas.cells.size(), treeAtlas.size,
                        treeAtlas.size);
            std::size_t billboardInstances = 0;

            auto quad = std::make_unique<genome::Mesh>();
            genome::MeshElement element;
            element.positions = {{-0.5f, 0.0f, 0.0f}, {0.5f, 0.0f, 0.0f}, {0.5f, 1.0f, 0.0f}, {-0.5f, 1.0f, 0.0f}};
            element.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
            element.indices = {0, 1, 2, 0, 2, 3};
            element.materialName = "tree billboard";
            quad->elements.push_back(std::move(element));

            for (std::size_t index = 0; index < treeFullDetail.size(); ++index)
            {
                const std::size_t slot = treeFullDetail[index];
                const std::array<float, 4> &cell = treeAtlas.cells[index];

                // One quad per definition, with that definition's cell baked
                // into its texture coordinates.
                auto mesh = std::make_unique<genome::Mesh>();
                genome::MeshElement card = quad->elements.front();
                card.texCoords = {{cell[0], cell[3]}, {cell[2], cell[3]}, {cell[2], cell[1]}, {cell[0], cell[1]}};
                mesh->elements.push_back(std::move(card));

                render::MeshInstances billboard;
                billboard.mesh = mesh.get();
                billboard.textures.push_back(&treeAtlasImage);
                billboard.alphaTested.push_back(1);
                billboard.faceCamera = true;
                billboard.occludes = false;
                billboard.lodNear = treeBillboardDistance;
                billboard.transforms = batches[slot].transforms;
                billboard.bounds = batches[slot].bounds;
                billboardInstances += billboard.transforms.size();
                batches.push_back(std::move(billboard));
                ownedMeshes.push_back(std::move(mesh));

                // The thinned tree now ends where the billboard begins.
                if (slot + 1 < batches.size())
                    batches[slot + 1].lodFar = treeBillboardDistance;
            }
            std::printf("billboards cover %zu tree instances beyond %.0f units\n", billboardInstances,
                        treeBillboardDistance);
        }
        else
            std::printf("warning: no tree billboards: %s\n", error.c_str());
        baker.destroy(device);
    }

    render::WorldRenderer renderer;
    if (!renderer.create(device, batches, &error))
    {
        std::cerr << "renderer: " << error << "\n";
        return 1;
    }

    std::printf("%zu instances lit by a baked lightmap, %zu without one, %zu colours in all\n", lightmapsFound,
                lightmapsMissing, lightmapColours.size());
    renderer.setLights(worldLights);
    renderer.setLightmaps(std::move(lightmapColours));

    // Before the loop: setting this up records and submits a command buffer of
    // its own to calibrate the card's clock against ours, which cannot happen
    // while a frame is being recorded.
    renderer.startProfiling(device);

    const std::array<float, 3> &min = renderer.boundsMin();
    const std::array<float, 3> &max = renderer.boundsMax();
    const std::array<float, 3> centre{(min[0] + max[0]) * 0.5f, (min[1] + max[1]) * 0.5f, (min[2] + max[2]) * 0.5f};
    const float span = std::max(max[0] - min[0], max[2] - min[2]);
    std::printf("world spans %.0f x %.0f x %.0f units (%.1f x %.1f km), %zu draws\n", max[0] - min[0],
                max[1] - min[1], max[2] - min[2], (max[0] - min[0]) / 100000.0f,
                (max[2] - min[2]) / 100000.0f, renderer.drawCount());

    // Spectator camera: look with the right mouse button held, move with WASD,
    // rise and fall with E and Q. Shift accelerates, and the speed scales with
    // the world so the same controls work on a hut and on the whole map.
    std::array<float, 3> eye{centre[0], centre[1] + span * 0.25f, centre[2] + span * 0.6f};
    // Start looking at what was loaded, whatever its size, rather than at a
    // fixed heading that only suits one scale.
    const std::array<float, 3> toCentre{centre[0] - eye[0], centre[1] - eye[1], centre[2] - eye[2]};
    float yaw = std::atan2(toCentre[0], toCentre[2]);
    float pitch = std::atan2(toCentre[1], std::sqrt(toCentre[0] * toCentre[0] + toCentre[2] * toCentre[2]));

    // An explicit viewpoint makes a picture repeatable, which the automatic
    // framing cannot be once the loaded extent changes.
    if (cameraArgument != 0)
    {
        eye = {float(std::atof(argv[cameraArgument])), float(std::atof(argv[cameraArgument + 1])),
               float(std::atof(argv[cameraArgument + 2]))};
        yaw = float(std::atof(argv[cameraArgument + 3])) * 3.14159265f / 180.0f;
        pitch = float(std::atof(argv[cameraArgument + 4])) * 3.14159265f / 180.0f;
        std::printf("camera at %.0f %.0f %.0f looking %s %s degrees\n", eye[0], eye[1], eye[2],
                    argv[cameraArgument + 3], argv[cameraArgument + 4]);
    }
    bool looking = false;
    bool occlusion = true;
    POINT lastCursor{};

    std::vector<float> frameTimes, cullTimes;
    if (benchFrames > 0)
    {
        frameTimes.reserve(std::size_t(benchFrames));
        cullTimes.reserve(std::size_t(benchFrames));
    }

    auto previous = std::chrono::steady_clock::now();
    std::size_t frames = 0;
    while (window.pump())
    {
        const auto now = std::chrono::steady_clock::now();
        const float delta = std::min(std::chrono::duration<float>(now - previous).count(), 0.1f);
        previous = now;

        const bool wantsLook = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        POINT cursor{};
        GetCursorPos(&cursor);
        if (wantsLook && looking)
        {
            yaw -= float(cursor.x - lastCursor.x) * 0.005f;
            pitch = std::clamp(pitch - float(cursor.y - lastCursor.y) * 0.005f, -1.5f, 1.5f);
        }
        looking = wantsLook;
        lastCursor = cursor;

        const std::array<float, 3> forward{std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                                           std::cos(pitch) * std::cos(yaw)};
        const std::array<float, 3> right{std::sin(yaw - 1.5708f), 0.0f, std::cos(yaw - 1.5708f)};

        float speed = span * 0.12f * delta;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
            speed *= 6.0f;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000)
            speed *= 0.15f;

        const auto move = [&](const std::array<float, 3> &direction, float scale) {
            for (int axis = 0; axis < 3; ++axis)
                eye[axis] += direction[axis] * scale;
        };
        if (window.keyDown('W'))
            move(forward, speed);
        if (window.keyDown('S'))
            move(forward, -speed);
        if (window.keyDown('D'))
            move(right, speed);
        if (window.keyDown('A'))
            move(right, -speed);
        if (window.keyDown('E'))
            eye[1] += speed;
        if (window.keyDown('Q'))
            eye[1] -= speed;
        if (window.keyPressed('O'))
            occlusion = !occlusion;

        const std::array<float, 3> target{eye[0] + forward[0], eye[1] + forward[1], eye[2] + forward[2]};

        const VkExtent2D extent = device.extent();
        const float aspect = float(extent.width) / float(extent.height);
        const std::array<float, 16> viewProjection =
            multiply(perspective(1.0f, aspect, 50.0f, span * 4.0f), lookAt(eye, target));

        if (!device.beginFrame())
            continue;

        VkCommandBuffer command = device.commandBuffer();

        VkImageMemoryBarrier toAttachment{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toAttachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toAttachment.image = device.currentColorImage();
        toAttachment.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toAttachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toAttachment);

        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = device.currentColorView();
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0.53f, 0.66f, 0.79f, 1.0f}}; // daylight sky

        VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depthAttachment.imageView = device.depthView();
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.clearValue.depthStencil = {1.0f, 0};

        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, extent};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        rendering.pDepthAttachment = &depthAttachment;
        vkCmdBeginRendering(command, &rendering);

        VkViewport viewport{0.0f, 0.0f, float(extent.width), float(extent.height), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetViewport(command, 0, 1, &viewport);
        vkCmdSetScissor(command, 0, 1, &scissor);

        // A pixel and a half: below that an object is a speck, whatever it is.
        const float pixelsPerRadian = float(extent.height) * 0.5f / std::tan(0.5f);
        const auto cullStart = std::chrono::steady_clock::now();
        renderer.cull(device, viewProjection, eye, pixelsPerRadian, 1.5f, occlusion);
        if (benchFrames > 0)
            cullTimes.push_back(std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() -
                                                                        cullStart).count());

        static float reportAt = 0.0f;
        reportAt += delta;
        if (reportAt > 1.0f)
        {
            reportAt = 0.0f;
            std::printf("visible %zu of %zu (%zu too small, %zu occluded%s)\n", renderer.visibleInstances(),
                        renderer.instanceCount(), renderer.tooSmallInstances(), renderer.occludedInstances(),
                        occlusion ? "" : ", occlusion off");
        }
        renderer.draw(device, viewProjection, {0.45f, 0.75f, 0.35f, 0.0f});

        vkCmdEndRendering(command);
        // Outside the render pass, which is where the profiler may write to its
        // query pool.
        renderer.collectProfiling(device);

        VkImageMemoryBarrier toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.image = device.currentColorImage();
        toPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toPresent.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &toPresent);

        device.endFrame();
        G3_FRAME_MARK;

        if (benchFrames > 0)
        {
            frameTimes.push_back(
                std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - now).count());
            if (int(frameTimes.size()) >= benchFrames)
            {
                // The first frames pay for pipeline warm-up and uploads, so they
                // are not what a steady frame costs.
                const std::size_t skip = std::min<std::size_t>(10, frameTimes.size() / 4);
                auto report = [skip](std::vector<float> &times, const char *label) {
                    std::vector<float> kept(times.begin() + skip, times.end());
                    std::sort(kept.begin(), kept.end());
                    float total = 0.0f;
                    for (float value : kept)
                        total += value;
                    std::printf("%-6s mean %6.2f ms   median %6.2f   95th %6.2f   worst %6.2f\n", label,
                                total / float(kept.size()), kept[kept.size() / 2],
                                kept[std::size_t(float(kept.size()) * 0.95f)], kept.back());
                };
                report(frameTimes, "frame");
                report(cullTimes, "cull");
                std::printf("%.2fM instances walked by the cull\n", double(renderer.testedInstances()) / 1e6);
                std::printf("%zu draws, %.2fM triangles submitted\n", renderer.submittedDraws(),
                            double(renderer.submittedTriangles()) / 1e6);
                std::printf("%zu of %zu instances drawn\n", renderer.visibleInstances(),
                            renderer.instanceCount());
                break;
            }
        }

        // Give the view a few frames to settle, then take the picture and go.
        if (shotPath && ++frames == 5)
        {
            std::string shotError;
            // The per-second report has not had a chance to fire this early, and
            // a capture is worth nothing without the numbers behind it.
            std::printf("visible %zu of %zu (%zu too small, %zu occluded)\n", renderer.visibleInstances(),
                        renderer.instanceCount(), renderer.tooSmallInstances(), renderer.occludedInstances());
            if (device.capture(shotPath, &shotError))
                std::printf("wrote %s\n", shotPath);
            else
                std::printf("capture failed: %s\n", shotError.c_str());
            break;
        }
    }

    vkDeviceWaitIdle(device.device());
    renderer.stopProfiling();
    renderer.destroy(device);
    render::destroyTreeAtlas(device, treeAtlas);
    device.destroy();
    return 0;
}
