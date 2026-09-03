#include "world_renderer.h"

#include "profile.h"

#include <cstdio>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <cassert>
#include <chrono>
#include <map>
#include <set>
#include <tuple>

namespace render
{
namespace
{

struct PushConstants
{
    std::array<float, 16> viewProjection;
    std::array<float, 4> light;
    float alphaTested = 0.0f;    // per draw, not per frame
    float normalStrength = 1.0f; // per frame
};

std::vector<char> readFile(const std::string &path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return {};
    const std::streamsize size = file.tellg();
    file.seekg(0);
    std::vector<char> buffer(static_cast<std::size_t>(size));
    file.read(buffer.data(), size);
    return buffer;
}

VkShaderModule loadShader(VkDevice device, const std::string &path)
{
    const std::vector<char> code = readFile(path);
    if (code.empty())
        return VK_NULL_HANDLE;

    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = code.size();
    info.pCode = reinterpret_cast<const std::uint32_t *>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &info, nullptr, &module);
    return module;
}

// Geometry belongs in the card's own memory. Written host-visible it stays in
// system RAM and every vertex the card fetches crosses the bus - at 15M vertices
// of 32 bytes that is hundreds of megabytes a frame, for data that never
// changes after loading. The instance buffers are a different case and stay
// host-visible: they are rewritten every frame by the cull.
bool uploadDeviceLocal(Device &device, const void *data, VkDeviceSize bytes, VkBufferUsageFlags usage,
                       Buffer &out, std::string *error)
{
    out = device.createBuffer(bytes, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, error);
    if (!out.handle)
        return false;

    Buffer staging = device.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, error);
    if (!staging.handle)
        return false;
    std::memcpy(staging.mapped, data, std::size_t(bytes));

    VkCommandBuffer command = device.beginOneShot();
    VkBufferCopy copy{0, 0, bytes};
    vkCmdCopyBuffer(command, staging.handle, out.handle, 1, &copy);
    device.endOneShot(command);
    device.destroyBuffer(staging);
    return true;
}

// A one-pixel stand-in so an untextured range still has something to sample.
genome::Image solidImage(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a)
{
    genome::Image image;
    image.width = 1;
    image.height = 1;
    image.faceCount = 1;
    image.format = genome::ImageFormat::A8R8G8B8;
    image.data = {b, g, r, a};
    image.levels.push_back({1, 1, 0, 4});
    image.faceStride = 4;
    return image;
}

} // namespace

bool WorldRenderer::create(Device &device, const Budget &budget, std::string *error)
{
    m_budget = budget;

    m_boundsMin = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max()};
    m_boundsMax = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                   std::numeric_limits<float>::lowest()};

    if (!m_vertices.create(device, sizeof(WorldVertex), budget.vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, error))
        return false;
    if (!m_indices.create(device, sizeof(std::uint32_t), budget.indices, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, error))
        return false;

    // The three lighting arenas are read as buffer[base + index], so they
    // suballocate exactly as the geometry does. Incident is three floats a
    // vertex and the patch coordinate two, both kept in floats so that neither
    // depends on the other being the length it should be.
    if (!m_lightmapArena.create(device, sizeof(std::uint32_t), budget.lightVertices,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, error))
        return false;
    if (!m_incidentArena.create(device, sizeof(float), 3 * budget.lightVertices, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                error))
        return false;
    if (!m_coordArena.create(device, sizeof(float), 2 * budget.lightVertices, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             error))
        return false;

    if (!m_uploader.create(device, budget.staging, error))
        return false;

    // One buffer per frame in flight: culling rewrites it every frame.
    for (Buffer &buffer : m_instanceBuffer)
    {
        buffer = device.createBuffer(sizeof(genome::WorldMatrix) * budget.instances,
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true, error);
        if (!buffer.handle)
            return false;
    }
    m_visible.reserve(budget.instances);

    const genome::Image white = solidImage(255, 255, 255, 255);
    if (!createTexture(device, white, true, m_white, error))
        return false;

    // Binding 0 the diffuse, binding 1 the normal map.
    VkDescriptorSetLayoutBinding bindings[2]{};
    for (int at = 0; at < 2; ++at)
    {
        bindings[at].binding = std::uint32_t(at);
        bindings[at].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[at].descriptorCount = 1;
        bindings[at].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    vkCreateDescriptorSetLayout(device.device(), &layoutInfo, nullptr, &m_descriptorLayout);

    // Sets are handed out as textures turn up rather than counted first, so the
    // pool is sized by the budget.
    const std::uint32_t setCount = budget.textures + 1;
    // Two images a set now.
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, setCount * 2};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    // Without this the spec forbids freeing a set back to the pool at all -
    // only vkResetDescriptorPool, which would invalidate every set at once.
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = setCount;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    vkCreateDescriptorPool(device.device(), &poolInfo, nullptr, &m_descriptorPool);

    // Flat in tangent space: 128,128,255 decodes to a normal straight out of
    // the surface. White would decode to one pointing nowhere useful.
    const genome::Image flat = solidImage(128, 128, 255, 255);
    if (!createTexture(device, flat, false, m_flatNormal, error))
        return false;

    m_whiteSet = acquireMaterial(device, nullptr, nullptr, error);
    if (!m_whiteSet)
        return false;

    return createPipeline(device, error);
}

// A texture per image, counted by how many sets name it. Split from the set so
// that two materials sharing a diffuse and differing in their normal map get
// two sets and one copy of the shared image.
Texture *WorldRenderer::textureFor(Device &device, const genome::Image *image, bool srgb, std::string *error)
{
    const auto existing = m_textureOf.find(image);
    if (existing != m_textureOf.end())
    {
        ++existing->second.refs;
        return &existing->second.texture;
    }

    Texture texture{};
    if (!createTexture(device, *image, srgb, texture, error))
        return nullptr;
    m_texturesRemade += m_everFreed.count(image) != 0 ? 1 : 0;
    return &m_textureOf.emplace(image, TextureEntry{texture, 1}).first->second.texture;
}

VkDescriptorSet WorldRenderer::acquireMaterial(Device &device, const genome::Image *diffuse,
                                               const genome::Image *normal, std::string *error)
{
    if (!diffuse && !normal && m_whiteSet)
        return m_whiteSet;

    const auto key = std::make_pair(diffuse, normal);
    const auto existing = m_setOf.find(key);
    if (existing != m_setOf.end())
    {
        ++existing->second.refs;
        return existing->second.set;
    }

    // The diffuse is colour and wants the sRGB curve; a normal map is data and
    // must not have one applied to it.
    Texture *colour = &m_white;
    if (diffuse)
    {
        colour = textureFor(device, diffuse, true, error);
        if (!colour)
            return VK_NULL_HANDLE;
    }
    Texture *bump = &m_flatNormal;
    if (normal)
    {
        bump = textureFor(device, normal, false, error);
        if (!bump)
        {
            releaseMaterial(diffuse, nullptr);
            return VK_NULL_HANDLE;
        }
    }

    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = m_descriptorPool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &m_descriptorLayout;
    const VkResult allocated = vkAllocateDescriptorSets(device.device(), &allocate, &set);
    if (allocated != VK_SUCCESS)
    {
        // With the free bit set the driver runs the pool as a general
        // allocator, so running out and fragmenting are different answers and
        // saying "raise the budget" to the second one would be wrong.
        if (error)
            *error = allocated == VK_ERROR_FRAGMENTED_POOL
                         ? "the descriptor pool is fragmented; sets are being freed and remade too finely"
                         : "the descriptor pool is out of sets; raise Budget::textures";
        releaseMaterial(diffuse, normal);
        return VK_NULL_HANDLE;
    }

    const VkDescriptorImageInfo images[2] = {
        {colour->sampler, colour->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {bump->sampler, bump->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[2]{};
    for (int at = 0; at < 2; ++at)
    {
        writes[at].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[at].dstSet = set;
        writes[at].dstBinding = std::uint32_t(at);
        writes[at].descriptorCount = 1;
        writes[at].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[at].pImageInfo = &images[at];
    }
    vkUpdateDescriptorSets(device.device(), 2, writes, 0, nullptr);

    m_setOf.emplace(key, SetEntry{set, 1});
    return set;
}

// Gives back one batch's hold on a pair. The set goes at zero, and each image
// goes when no set names it any more. Both wait out the frames in flight rather
// than being destroyed here: a submitted frame may still be binding them.
void WorldRenderer::releaseMaterial(const genome::Image *diffuse, const genome::Image *normal)
{
    // The shared white set is handed out without being counted - acquire
    // returns it before it ever reaches the map - so counting it here would
    // take it to zero and destroy it while every untextured range still binds
    // it. Which is what happened, and what the validation layers said.
    if (!diffuse && !normal)
        return;

    const auto held = m_setOf.find(std::make_pair(diffuse, normal));
    if (held == m_setOf.end() || held->second.refs == 0 || --held->second.refs != 0)
        return;

    m_retiringTextures.push_back({Texture{}, held->second.set, m_frameCounter});
    m_setOf.erase(held);

    // The images are taken once, when the set is made, so they are given back
    // once, when it goes - not on every release. Giving them back per release
    // destroyed an image while a set two ranges shared was still binding it,
    // which is what the validation layers reported.
    for (const genome::Image *image : {diffuse, normal})
    {
        if (!image)
            continue;
        const auto found = m_textureOf.find(image);
        if (found == m_textureOf.end() || found->second.refs == 0)
            continue;
        if (--found->second.refs != 0)
            continue;
        m_retiringTextures.push_back({found->second.texture, VK_NULL_HANDLE, m_frameCounter});
        m_everFreed.insert(image);
        m_textureOf.erase(found);
    }
}

// Puts a mesh in the arenas, or finds the copy already there. The element table
// is rebased once here, so every batch drawing this mesh reuses the same draw
// parameters rather than working them out again.
bool WorldRenderer::placeMesh(Device &device, const genome::Mesh &mesh, MeshGeometry *&out, std::string *error)
{
    const auto started = std::chrono::steady_clock::now();
    struct Timed
    {
        double &into;
        std::chrono::steady_clock::time_point start;
        ~Timed() { into += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); }
    } timed{m_secondsPlacing, started};

    const auto existing = m_geometry.find(&mesh);
    if (existing != m_geometry.end())
    {
        ++existing->second.refs;
        out = &existing->second;
        return true;
    }

    std::size_t vertexCount = 0, indexCount = 0;
    for (const genome::MeshElement &element : mesh.elements)
    {
        if (element.positions.empty() || element.indices.empty())
            continue;
        vertexCount += element.positions.size();
        indexCount += element.indices.size();
    }
    if (vertexCount == 0)
    {
        out = nullptr;
        return true;
    }

    MeshGeometry geometry;
    geometry.refs = 1;
    geometry.vertexCount = vertexCount;
    geometry.indexCount = indexCount;
    geometry.vertexOffset = m_vertices.allocate(vertexCount);
    geometry.indexOffset = m_indices.allocate(indexCount);
    if (geometry.vertexOffset == GpuArena::npos || geometry.indexOffset == GpuArena::npos)
    {
        // One of the two may have succeeded, and nothing else knows about it -
        // this mesh never reaches m_geometry, so no later release can find it.
        // Straight back to the free list rather than through queueRelease: no
        // frame can be reading a range that was never uploaded to.
        if (geometry.vertexOffset != GpuArena::npos)
            m_vertices.release(geometry.vertexOffset, vertexCount);
        if (geometry.indexOffset != GpuArena::npos)
            m_indices.release(geometry.indexOffset, indexCount);
        if (error)
            *error = "the geometry arena is full; raise Budget::vertices or Budget::indices";
        return false;
    }

    // Built once and handed over together, so a mesh is one staged copy of
    // vertices and one of indices however many elements it has.
    std::vector<WorldVertex> vertices;
    std::vector<std::uint32_t> indices;
    vertices.reserve(vertexCount);
    indices.reserve(indexCount);

    for (const genome::MeshElement &element : mesh.elements)
    {
        if (element.positions.empty() || element.indices.empty())
            continue;

        MeshGeometry::Element out{};
        out.firstIndex = std::uint32_t(geometry.indexOffset + indices.size());
        out.indexCount = std::uint32_t(element.indices.size());
        out.vertexOffset = std::int32_t(geometry.vertexOffset + vertices.size());
        geometry.elements.push_back(out);

        // Indices stay local to their element; vertexOffset does the rebasing
        // at draw time, so a mesh is stored once however often it is placed.
        indices.insert(indices.end(), element.indices.begin(), element.indices.end());

        for (std::size_t index = 0; index < element.positions.size(); ++index)
        {
            WorldVertex vertex{};
            vertex.position = element.positions[index];
            vertex.normal =
                index < element.normals.size() ? element.normals[index] : std::array<float, 3>{0.0f, 1.0f, 0.0f};
            vertex.texCoord =
                index < element.texCoords.size() ? element.texCoords[index] : std::array<float, 2>{0.0f, 0.0f};
            vertices.push_back(vertex);
        }
    }

    if (!m_uploader.write(device, m_vertices, geometry.vertexOffset, vertices.data(), vertices.size(), error) ||
        !m_uploader.write(device, m_indices, geometry.indexOffset, indices.data(), indices.size(), error))
    {
        m_vertices.release(geometry.vertexOffset, vertexCount);
        m_indices.release(geometry.indexOffset, indexCount);
        return false;
    }

    m_vertexCount += vertexCount;
    m_indexCount += indexCount;
    out = &m_geometry.emplace(&mesh, std::move(geometry)).first->second;
    return true;
}

void WorldRenderer::queueRelease(GpuArena &arena, std::size_t offset, std::size_t count)
{
    if (count != 0)
        m_pendingReleases.push_back({&arena, offset, count, m_frameCounter});
}

void WorldRenderer::retireReleases(Device &device, std::uint64_t frame)
{
    m_frameCounter = frame;
    std::size_t retiredHere = 0;
    for (std::size_t index = 0; index < m_pendingReleases.size();)
    {
        const PendingRelease &one = m_pendingReleases[index];
        // One more than the frames in flight, because the release was recorded
        // between frames rather than inside one.
        if (frame < one.frame + Device::c_FramesInFlight + 1)
        {
            ++index;
            continue;
        }
        one.arena->release(one.offset, one.count);
        m_pendingReleases.erase(m_pendingReleases.begin() + std::ptrdiff_t(index));
    }

    for (std::size_t index = 0; index < m_retiringTextures.size() && retiredHere < c_RetirePerFrame;)
    {
        RetiringTexture &one = m_retiringTextures[index];
        if (frame < one.frame + Device::c_FramesInFlight + 1)
        {
            ++index;
            continue;
        }
        if (one.set != VK_NULL_HANDLE)
            vkFreeDescriptorSets(device.device(), m_descriptorPool, 1, &one.set);
        if (one.texture.valid())
        {
            destroyTexture(device, one.texture);
            ++m_texturesFreed;
        }
        ++retiredHere;
        m_retiringTextures.erase(m_retiringTextures.begin() + std::ptrdiff_t(index));
    }

    m_worstRetireBurst = std::max(m_worstRetireBurst, retiredHere);
}

void WorldRenderer::releaseMesh(Device &device, const genome::Mesh *mesh)
{
    (void)device;
    const auto found = m_geometry.find(mesh);
    if (found == m_geometry.end() || found->second.refs == 0)
        return;
    if (--found->second.refs != 0)
        return;

    queueRelease(m_vertices, found->second.vertexOffset, found->second.vertexCount);
    queueRelease(m_indices, found->second.indexOffset, found->second.indexCount);
    m_vertexCount -= found->second.vertexCount;
    m_indexCount -= found->second.indexCount;
    m_geometry.erase(found);
}

bool WorldRenderer::sectorResident(std::uint32_t sector) const
{
    for (const Sector &held : m_sectors)
        if (held.id == sector)
            return true;
    return false;
}

bool WorldRenderer::updatePatchAtlas(Device &device, const std::vector<TextureRegion> &regions, std::string *error)
{
    if (!m_lightmapTexture.valid())
        return true;
    return updateTextureRegions(device, m_lightmapTexture, regions, error);
}

namespace
{

// A mesh's own box, in the space its vertices are stored in.
std::array<float, 6> objectBox(const genome::Mesh &mesh)
{
    std::array<float, 6> box{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest(),
                             std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    for (const genome::MeshElement &element : mesh.elements)
        for (const std::array<float, 3> &position : element.positions)
            for (int axis = 0; axis < 3; ++axis)
            {
                box[axis] = std::min(box[axis], position[axis]);
                box[axis + 3] = std::max(box[axis + 3], position[axis]);
            }
    return box;
}

// That box put where the instance is, by its eight corners. A superset of the
// transformed vertices, and vastly cheaper than walking them.
std::array<float, 6> worldBox(const std::array<float, 6> &local, const genome::WorldMatrix &m)
{
    std::array<float, 6> box{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest(),
                             std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    for (int corner = 0; corner < 8; ++corner)
    {
        const std::array<float, 3> v{local[(corner & 1) ? 3 : 0], local[(corner & 2) ? 4 : 1],
                                     local[(corner & 4) ? 5 : 2]};
        const std::array<float, 3> at{v[0] * m[0] + v[1] * m[4] + v[2] * m[8] + m[12],
                                      v[0] * m[1] + v[1] * m[5] + v[2] * m[9] + m[13],
                                      v[0] * m[2] + v[1] * m[6] + v[2] * m[10] + m[14]};
        for (int axis = 0; axis < 3; ++axis)
        {
            box[axis] = std::min(box[axis], at[axis]);
            box[axis + 3] = std::max(box[axis + 3], at[axis]);
        }
    }
    return box;
}

} // namespace

bool WorldRenderer::addSector(Device &device, std::uint32_t sector, const std::vector<MeshInstances> &batches,
                              const SectorLighting &lighting, std::string *error)
{
    // m_batchOf indexes m_batches, and a departure earlier in this same frame
    // erased from it - which shifts every later index and leaves the map
    // pointing at the wrong batch, or past the end. The cull that would have
    // rebuilt it does not run until after this.
    if (m_derivedStale)
    {
        // What the lookup would have returned had the rebuild not happened:
        // an index past the end, or the wrong mesh's batch.
        ++m_staleArrivals;
        for (const MeshInstances &incoming : batches)
        {
            if (!incoming.mesh)
                continue;
            const auto stale = m_batchOf.find(incoming.mesh);
            if (stale == m_batchOf.end())
                continue;
            if (stale->second >= m_batches.size())
                ++m_staleOutOfRange;
            else if (m_batches[stale->second].mesh != incoming.mesh)
                ++m_staleLookups;
        }
    }
    ensureDerived();

    Sector held;
    held.id = sector;
    held.colourCount = lighting.colours.size();

    // One allocator for three buffers. The shader reads the colour, the
    // direction the light came from and the patch coordinate from the single
    // base an instance carries, so the other two have to sit at three and two
    // times the colour offset. Allocating them separately would let them drift
    // apart, and the shader has no way of being told.
    if (held.colourCount != 0)
    {
        held.colourBase = m_lightmapArena.allocate(held.colourCount);
        if (held.colourBase == GpuArena::npos)
        {
            if (error)
                *error = "the lighting arena is full; raise Budget::lightVertices";
            return false;
        }

        // The base reaches the shader through a float - world.vert reads it as
        // int(inRow0.w) and adds gl_VertexIndex - so it has to be
        // integer-exact, and a float is exact only to 2^24. Past that an
        // instance samples a neighbouring vertex's colour and patch
        // coordinate, which is a quietly wrong picture. Say so instead.
        constexpr std::size_t c_ExactInFloat = 1u << 24;
        if (held.colourBase + held.colourCount > c_ExactInFloat)
        {
            m_lightmapArena.release(held.colourBase, held.colourCount);
            if (error)
                *error = "the baked light passed 2^24 vertices, which a float base cannot address exactly";
            return false;
        }
    }

    // From here the sector is a thing that can be dropped, so every failure
    // below undoes itself by dropping it rather than by unwinding by hand. The
    // staged copies go too: flushing them afterwards would write into ranges
    // that are on their way back to the free list.
    m_sectors.push_back(held);
    const auto giveUp = [&]() {
        m_uploader.discard();
        dropSector(device, sector);
        return false;
    };

    if (held.colourCount != 0)
    {
        if (!m_uploader.write(device, m_lightmapArena, held.colourBase, lighting.colours.data(), held.colourCount,
                              error))
            return giveUp();
        if (!lighting.incident.empty() &&
            !m_uploader.write(device, m_incidentArena, 3 * held.colourBase, lighting.incident.data(),
                              std::min(lighting.incident.size(), 3 * held.colourCount), error))
            return giveUp();
        if (!lighting.coords.empty() &&
            !m_uploader.write(device, m_coordArena, 2 * held.colourBase, lighting.coords.data(),
                              std::min(lighting.coords.size(), 2 * held.colourCount), error))
            return giveUp();
    }

    for (const MeshInstances &incoming : batches)
    {
        if (!incoming.mesh || incoming.transforms.empty())
            continue;

        Batch *batch = nullptr;
        const auto known = m_batchOf.find(incoming.mesh);
        if (known != m_batchOf.end())
            batch = &m_batches[known->second];
        else
        {
            MeshGeometry *geometry = nullptr;
            if (!placeMesh(device, *incoming.mesh, geometry, error))
                return giveUp();
            if (!geometry)
                continue;

            Batch fresh;
            fresh.mesh = incoming.mesh;
            fresh.occludes = incoming.occludes;
            fresh.lodNear = incoming.lodNear;
            fresh.lodFar = incoming.lodFar;
            fresh.faceCamera = incoming.faceCamera;
            fresh.meshFirstVertex = std::int32_t(geometry->vertexOffset);
            fresh.localBox = objectBox(*incoming.mesh);

            std::size_t elementIndex = 0, drawable = 0;
            for (const genome::MeshElement &element : incoming.mesh->elements)
            {
                if (element.positions.empty() || element.indices.empty())
                {
                    ++elementIndex;
                    continue;
                }

                const MeshGeometry::Element &placed = geometry->elements[drawable++];
                Range range;
                range.firstIndex = placed.firstIndex;
                range.indexCount = placed.indexCount;
                range.vertexOffset = placed.vertexOffset;
                range.alphaTested = elementIndex < incoming.alphaTested.size() && incoming.alphaTested[elementIndex] != 0;

                const genome::Image *image =
                    elementIndex < incoming.textures.size() ? incoming.textures[elementIndex] : nullptr;
                range.image = image;
                range.normal = elementIndex < incoming.normals.size() ? incoming.normals[elementIndex] : nullptr;
                range.descriptor = acquireMaterial(device, range.image, range.normal, error);
                if (!range.descriptor)
                {
                    // This batch was never pushed, so dropSector cannot see it:
                    // its ranges, their textures and its hold on the mesh are
                    // all undone here.
                    for (const Range &taken : fresh.ranges)
                        releaseMaterial(taken.image, taken.normal);
                    m_rangeCount -= fresh.ranges.size();
                    releaseMesh(device, incoming.mesh);
                    return giveUp();
                }

                fresh.ranges.push_back(range);
                ++m_rangeCount;
                ++elementIndex;
            }

            m_batchOf.emplace(incoming.mesh, m_batches.size());
            m_batches.push_back(std::move(fresh));
            batch = &m_batches.back();
        }

        for (std::size_t index = 0; index < incoming.transforms.size(); ++index)
        {
            // Two unused corners of the transform carry the lighting: the first
            // row's w holds the base, which may be negative, and the second
            // row's w says whether there is any at all - a sentinel in the base
            // would be indistinguishable from a legitimate negative one.
            const bool lit = index < incoming.lightmapBase.size() && incoming.lightmapBase[index] >= 0;
            genome::WorldMatrix m = incoming.transforms[index];
            m[3] = lit ? float(std::int64_t(held.colourBase) + incoming.lightmapBase[index] - batch->meshFirstVertex)
                       : 0.0f;
            m[7] = lit ? 1.0f : 0.0f;
            m_bakedInstances += lit ? 1 : 0;

            batch->transforms.push_back(m);
            batch->sectorOf.push_back(sector);
            if (index < incoming.bounds.size())
                batch->bounds.push_back(incoming.bounds[index]);
        }
    }

    const auto flushStart = std::chrono::steady_clock::now();
    if (!m_uploader.flush(device, error))
        return giveUp();
    m_secondsFlushing += std::chrono::duration<double>(std::chrono::steady_clock::now() - flushStart).count();

    m_derivedStale = true;

    // The instance count is one of the things the rebuild works out, so the
    // check that the buffer can hold them has to happen after one. The copies
    // are already sent by now, so there is nothing left to discard - but the
    // sector still has to come back out.
    ensureDerived();
    if (m_instanceCount > m_budget.instances)
    {
        if (error)
            *error = "more instances than the buffer holds; raise Budget::instances";
        dropSector(device, sector);
        return false;
    }
    return true;
}

void WorldRenderer::dropSector(Device &device, std::uint32_t sector)
{
    const auto held = std::find_if(m_sectors.begin(), m_sectors.end(),
                                   [sector](const Sector &one) { return one.id == sector; });
    if (held == m_sectors.end())
        return;

    if (held->colourCount != 0)
        queueRelease(m_lightmapArena, held->colourBase, held->colourCount);
    m_sectors.erase(held);

    for (std::size_t index = 0; index < m_batches.size();)
    {
        Batch &batch = m_batches[index];

        // Bounds are meant to run in step with the transforms. Where they do
        // not - a landscape tile placed once with no box of its own - they
        // cannot be compacted alongside, so they go, and the batch falls back
        // to being tested per instance.
        const bool parallel = batch.bounds.size() == batch.transforms.size();
        std::size_t write = 0;
        bool removedAny = false;
        for (std::size_t at = 0; at < batch.transforms.size(); ++at)
        {
            if (batch.sectorOf[at] == sector)
            {
                removedAny = true;
                // row1.w is the has-lighting flag the shader reads, so it is
                // also what says whether this instance was counted as lit.
                m_bakedInstances -= (batch.transforms[at][7] != 0.0f && m_bakedInstances != 0) ? 1 : 0;
                continue;
            }
            batch.transforms[write] = batch.transforms[at];
            batch.sectorOf[write] = batch.sectorOf[at];
            if (parallel)
                batch.bounds[write] = batch.bounds[at];
            ++write;
        }
        if (!removedAny)
        {
            ++index;
            continue;
        }

        batch.transforms.resize(write);
        batch.sectorOf.resize(write);
        if (parallel)
            batch.bounds.resize(write);
        else
            batch.bounds.clear();

        if (!batch.transforms.empty())
        {
            ++index;
            continue;
        }

        // Nothing is drawing this mesh any more, so the arenas get it back,
        // and so do the textures its ranges were holding.
        for (const Range &range : batch.ranges)
            releaseMaterial(range.image, range.normal);
        releaseMesh(device, batch.mesh);
        m_rangeCount -= batch.ranges.size();
        m_batches.erase(m_batches.begin() + std::ptrdiff_t(index));
    }

    m_derivedStale = true;
}

// Rebuilds them if anything moved. Called before every cull, and by anything
// that needs a number the rebuild produces.
void WorldRenderer::ensureDerived()
{
    if (!m_derivedStale)
        return;
    m_derivedStale = false;

    const auto started = std::chrono::steady_clock::now();
    rebuildDerived();
    m_secondsRebuilding += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

// Extents, world bounds, occluders, the grid and the mesh-to-batch map are all
// views of the batch list, so they are worked out again whenever it changes
// rather than patched in a dozen places.
std::vector<float> WorldRenderer::threadBusy() const
{
    std::vector<float> busy;
    busy.reserve(m_cullCounts.size());
    for (const CullCounts &counts : m_cullCounts)
        busy.push_back(float(counts.seconds * 1000.0));
    std::sort(busy.begin(), busy.end(), std::greater<float>());
    return busy;
}

void WorldRenderer::setCullThreads(unsigned threads)
{
    m_pool = std::make_unique<Pool>(threads);
}

void WorldRenderer::rebuildDerived()
{
    // Each batch's block in the instance buffer, laid out in batch order and
    // sized for every instance it holds. Done here because this is the one
    // place the batch list settles.
    {
        std::size_t base = 0;
        for (Batch &batch : m_batches)
        {
            batch.instanceBase = base;
            base += batch.transforms.size();
        }
    }

    // Heaviest first. Eight threads were returning four because the wall was
    // one thread still holding the biggest batch after the others had run out
    // of work; starting with it puts that thread's tail in the middle of
    // everyone else's rather than past the end.
    m_cullOrder.resize(m_batches.size());
    for (std::size_t index = 0; index < m_cullOrder.size(); ++index)
        m_cullOrder[index] = index;
    std::sort(m_cullOrder.begin(), m_cullOrder.end(), [this](std::size_t left, std::size_t right) {
        return m_batches[left].transforms.size() > m_batches[right].transforms.size();
    });

    m_batchOf.clear();
    m_occluders.clear();
    m_foliageSkipped = 0;
    m_instanceCount = 0;
    m_boundsMin = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max()};
    m_boundsMax = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                   std::numeric_limits<float>::lowest()};

    for (std::size_t index = 0; index < m_batches.size(); ++index)
    {
        Batch &batch = m_batches[index];
        m_batchOf.emplace(batch.mesh, index);
        m_instanceCount += batch.transforms.size();

        // Only useful for rejecting the batch whole when every instance has
        // bounds; otherwise some are drawn unconditionally.
        batch.hasExtent = !batch.bounds.empty() && batch.bounds.size() == batch.transforms.size();

        bool first = true;
        const auto grow = [&](const std::array<float, 6> &box) {
            if (first)
            {
                batch.extent = box;
                first = false;
                return;
            }
            for (int axis = 0; axis < 3; ++axis)
            {
                batch.extent[axis] = std::min(batch.extent[axis], box[axis]);
                batch.extent[axis + 3] = std::max(batch.extent[axis + 3], box[axis + 3]);
            }
        };

        if (batch.hasExtent)
            for (const std::array<float, 6> &box : batch.bounds)
                grow(box);
        else
            for (const genome::WorldMatrix &m : batch.transforms)
                grow(worldBox(batch.localBox, m));

        if (!first)
            for (int axis = 0; axis < 3; ++axis)
            {
                m_boundsMin[axis] = std::min(m_boundsMin[axis], batch.extent[axis]);
                m_boundsMax[axis] = std::max(m_boundsMax[axis], batch.extent[axis + 3]);
            }

        // Anything spanning more than about ten metres is worth rasterising as
        // an occluder: houses, cliffs and the landscape itself, not barrels. A
        // box is a fair stand-in for a house and a poor one for a tree, whose
        // box is mostly air, so foliage is drawn but never used to reject.
        for (std::size_t instance = 0; instance < batch.bounds.size(); ++instance)
        {
            const std::array<float, 6> &box = batch.bounds[instance];
            const float extent = std::max({box[3] - box[0], box[4] - box[1], box[5] - box[2]});
            if (extent < 1000.0f)
                continue;
            if (batch.occludes)
                m_occluders.push_back({index, instance});
            else
                ++m_foliageSkipped;
        }
    }

    buildGrid();
}

bool WorldRenderer::create(Device &device, const std::vector<MeshInstances> &batches, std::string *error)
{
    // A fixed set that will never be added to, so the budget is exactly what it
    // needs plus a little slack rather than a guess.
    std::size_t vertices = 0, indices = 0, instances = 0;
    std::set<const genome::Mesh *> seen;
    for (const MeshInstances &batch : batches)
    {
        instances += batch.transforms.size();
        if (!batch.mesh || !seen.insert(batch.mesh).second)
            continue;
        for (const genome::MeshElement &element : batch.mesh->elements)
        {
            if (element.positions.empty() || element.indices.empty())
                continue;
            vertices += element.positions.size();
            indices += element.indices.size();
        }
    }

    Budget budget;
    budget.vertices = vertices + 1024;
    budget.indices = indices + 1024;
    budget.lightVertices = std::max<std::size_t>(m_lightmapColours.size(), 1024);
    budget.instances = instances + 1024;
    budget.staging = std::min<VkDeviceSize>(64u << 20, sizeof(WorldVertex) * budget.vertices + (1u << 20));

    if (!create(device, budget, error))
        return false;

    // Everything at once is one sector, numbered zero. Nothing else knows the
    // difference, which is the point: there is one path in.
    SectorLighting lighting;
    lighting.colours = std::move(m_lightmapColours);
    lighting.incident = std::move(m_lightmapIncident);
    lighting.coords = std::move(m_lightmapCoords);
    if (!addSector(device, 0, batches, lighting, error))
        return false;

    if (m_vertexCount == 0 || m_instanceCount == 0)
    {
        if (error)
            *error = "no geometry to draw";
        return false;
    }
    return true;
}

VkDeviceSize WorldRenderer::textureBytes() const
{
    VkDeviceSize bytes = 0;
    for (const auto &[image, entry] : m_textureOf)
        bytes += entry.texture.bytes;
    return bytes;
}

void WorldRenderer::reportArenas() const
{
    std::printf("textures hold %.0f MB\n", double(textureBytes()) / 1048576.0);
    std::printf("arenas: %.0f%% of %zuM vertices, %.0f%% of %zuM indices, %.0f%% of %zuM lit vertices, "
                "%zu textures, %zu staging submits\n",
                100.0 * double(m_vertices.highWater()) / double(m_vertices.capacity()), m_vertices.capacity() >> 20,
                100.0 * double(m_indices.highWater()) / double(m_indices.capacity()), m_indices.capacity() >> 20,
                100.0 * double(m_lightmapArena.highWater()) / double(m_lightmapArena.capacity()),
                m_lightmapArena.capacity() >> 20, m_textureOf.size(), m_uploader.submits());
    std::printf("%zu sectors resident, %zu batches, %zu instances of which %zu carry baked light\n",
                m_sectors.size(), m_batches.size(), m_instanceCount, m_bakedInstances);
    std::printf("%zu occluders, %zu large enough but foliage\n", m_occluders.size(), m_foliageSkipped);
    // How the work the cull walks is distributed, which is what decides
    // whether dividing it by batch balances at all.
    {
        std::vector<std::size_t> sizes;
        std::size_t total = 0;
        for (const Batch &batch : m_batches)
        {
            sizes.push_back(batch.transforms.size());
            total += batch.transforms.size();
        }
        std::sort(sizes.begin(), sizes.end(), std::greater<std::size_t>());
        std::size_t biggestTen = 0;
        for (std::size_t at = 0; at < sizes.size() && at < 10; ++at)
            biggestTen += sizes[at];
        std::printf("%zu batches hold %zu instances: biggest %zu, median %zu, the ten biggest are %.0f%%\n",
                    sizes.size(), total, sizes.empty() ? std::size_t(0) : sizes.front(),
                    sizes.empty() ? std::size_t(0) : sizes[sizes.size() / 2],
                    total != 0 ? 100.0 * double(biggestTen) / double(total) : 0.0);
    }

    std::printf("addSector spent %.0f ms building mesh arrays, %.0f ms rebuilding the grid, %.0f ms in the queue\n",
                m_secondsPlacing * 1000.0, m_secondsRebuilding * 1000.0, m_secondsFlushing * 1000.0);
}

bool WorldRenderer::createPipeline(Device &device, std::string *error)
{
    VkShaderModule vertexShader = loadShader(device.device(), "shaders/world.vert.spv");
    VkShaderModule fragmentShader = loadShader(device.device(), "shaders/world.frag.spv");
    if (!vertexShader || !fragmentShader)
    {
        if (error)
            *error = "cannot load shaders/world.{vert,frag}.spv";
        return false;
    }

    // Set 1: the lights near the camera this frame. One buffer per frame in
    // flight, because the cull rewrites it while the previous frame may still be
    // reading the last one.
    // The baked lighting: one colour per vertex per instance, indexed by the
    // base the transform carries. A storage buffer because it is far past what a
    // uniform block may hold - a single sector runs to hundreds of thousands.
    // The baked light itself arrives with the sectors that carry it; the
    // pipeline only has to know where to look for it.

    // The atlas the patches were packed into. Without one, a single white texel
    // stands in and every lookup returns it.
    const genome::Image white1 = solidImage(255, 255, 255, 255);
    if (!createTexture(device, m_lightmapAtlas ? *m_lightmapAtlas : white1, true, m_lightmapTexture, error))
        return false;

    VkDescriptorSetLayoutBinding coordBinding{};
    coordBinding.binding = 3;
    coordBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    coordBinding.descriptorCount = 1;
    coordBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutBinding atlasBinding{};
    atlasBinding.binding = 4;
    atlasBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    atlasBinding.descriptorCount = 1;
    atlasBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutBinding incidentBinding{};
    incidentBinding.binding = 2;
    incidentBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    incidentBinding.descriptorCount = 1;
    incidentBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutBinding lightmapBinding{};
    lightmapBinding.binding = 1;
    lightmapBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    lightmapBinding.descriptorCount = 1;
    lightmapBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutBinding lightBinding{};
    lightBinding.binding = 0;
    lightBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    lightBinding.descriptorCount = 1;
    lightBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    const VkDescriptorSetLayoutBinding lightSetBindings[5] = {lightBinding, lightmapBinding, incidentBinding,
                                                              coordBinding, atlasBinding};
    VkDescriptorSetLayoutCreateInfo lightLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lightLayoutInfo.bindingCount = 5;
    lightLayoutInfo.pBindings = lightSetBindings;
    vkCreateDescriptorSetLayout(device.device(), &lightLayoutInfo, nullptr, &m_lightLayout);

    const VkDescriptorPoolSize lightPoolSizes[3] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, Device::c_FramesInFlight},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Device::c_FramesInFlight * 3},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, Device::c_FramesInFlight}};
    VkDescriptorPoolCreateInfo lightPoolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    lightPoolInfo.maxSets = Device::c_FramesInFlight;
    lightPoolInfo.poolSizeCount = 3;
    lightPoolInfo.pPoolSizes = lightPoolSizes;
    VkDescriptorPool lightPool = VK_NULL_HANDLE;
    vkCreateDescriptorPool(device.device(), &lightPoolInfo, nullptr, &lightPool);
    m_lightPool = lightPool;

    // Four floats of count and ambient, then position and range, then colour.
    const VkDeviceSize lightBytes = 16 + VkDeviceSize(c_MaxFrameLights) * 32;
    for (std::uint32_t frame = 0; frame < Device::c_FramesInFlight; ++frame)
    {
        m_lightBuffer[frame] = device.createBuffer(lightBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, error);
        if (!m_lightBuffer[frame].handle)
            return false;
        std::memset(m_lightBuffer[frame].mapped, 0, std::size_t(lightBytes));

        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = lightPool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &m_lightLayout;
        vkAllocateDescriptorSets(device.device(), &allocate, &m_lightSet[frame]);

        VkDescriptorBufferInfo bufferInfo{m_lightBuffer[frame].handle, 0, lightBytes};
        VkDescriptorBufferInfo lightmapInfo{m_lightmapArena.handle(), 0, VK_WHOLE_SIZE};

        VkDescriptorBufferInfo incidentInfo{m_incidentArena.handle(), 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo coordInfo{m_coordArena.handle(), 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo atlasInfo{m_lightmapTexture.sampler, m_lightmapTexture.view,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet writes[5]{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[0].dstSet = m_lightSet[frame];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &bufferInfo;

        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[1].dstSet = m_lightSet[frame];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &lightmapInfo;

        writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[2].dstSet = m_lightSet[frame];
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &incidentInfo;

        writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[3].dstSet = m_lightSet[frame];
        writes[3].dstBinding = 3;
        writes[3].descriptorCount = 1;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[3].pBufferInfo = &coordInfo;

        writes[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[4].dstSet = m_lightSet[frame];
        writes[4].dstBinding = 4;
        writes[4].descriptorCount = 1;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[4].pImageInfo = &atlasInfo;

        vkUpdateDescriptorSets(device.device(), 5, writes, 0, nullptr);
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    const VkDescriptorSetLayout setLayouts[2] = {m_descriptorLayout, m_lightLayout};
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = setLayouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    vkCreatePipelineLayout(device.device(), &layoutInfo, nullptr, &m_layout);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertexShader;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragmentShader;
    stages[1].pName = "main";

    // Binding 1 advances once per instance and carries the four rows of the
    // world matrix, which is how one mesh lands in a thousand places.
    VkVertexInputBindingDescription bindings[2]{
        {0, sizeof(WorldVertex), VK_VERTEX_INPUT_RATE_VERTEX},
        {1, sizeof(genome::WorldMatrix), VK_VERTEX_INPUT_RATE_INSTANCE},
    };
    VkVertexInputAttributeDescription attributes[7]{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(WorldVertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(WorldVertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(WorldVertex, texCoord)},
        {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
        {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
        {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32},
        {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 48},
    };

    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 2;
    vertexInput.pVertexBindingDescriptions = bindings;
    vertexInput.vertexAttributeDescriptionCount = 7;
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                                     VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    const VkFormat colorFormat = device.colorFormat();
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &colorFormat;
    rendering.depthAttachmentFormat = device.depthFormat();

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = m_layout;

    const VkResult result = vkCreateGraphicsPipelines(device.device(), VK_NULL_HANDLE, 1, &info, nullptr, &m_pipeline);
    vkDestroyShaderModule(device.device(), vertexShader, nullptr);
    vkDestroyShaderModule(device.device(), fragmentShader, nullptr);

    if (result != VK_SUCCESS)
    {
        if (error)
            *error = "vkCreateGraphicsPipelines failed";
        return false;
    }
    return true;
}

void WorldRenderer::cull(Device &device, const std::array<float, 16> &viewProjection,
                         const std::array<float, 3> &eye, float pixelsPerRadian, float minimumPixels,
                         bool useOcclusion)
{
    const auto prologueStart = std::chrono::steady_clock::now();
    retireReleases(device, device.frameCounter());
    ensureDerived();
    m_cullPhases.prologue = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() -
                                                                     prologueStart).count();

    G3_ZONE("cull");

    // Frustum planes straight out of the view-projection matrix: each is a
    // combination of two of its rows, in the same column-major layout the
    // shader receives.
    const auto plane = [&](int index) {
        std::array<float, 4> p{};
        const int row = index / 2;
        const float sign = (index % 2) == 0 ? 1.0f : -1.0f;
        for (int component = 0; component < 4; ++component)
            p[component] = viewProjection[component * 4 + 3] + sign * viewProjection[component * 4 + row];
        return p;
    };

    std::array<std::array<float, 4>, 6> planes{plane(0), plane(1), plane(2), plane(3), plane(4), plane(5)};

    // Testing what a batch covers before testing its instances. Most of the map
    // is grass and each patch is one batch of its own, local to a few metres, so
    // a single test throws away every instance in it.
    const auto outsideFrustum = [&planes](const std::array<float, 6> &box) {
        for (const std::array<float, 4> &p : planes)
        {
            const float x = p[0] >= 0.0f ? box[3] : box[0];
            const float y = p[1] >= 0.0f ? box[4] : box[1];
            const float z = p[2] >= 0.0f ? box[5] : box[2];
            if (p[0] * x + p[1] * y + p[2] * z + p[3] < 0.0f)
                return true;
        }
        return false;
    };

    // Comparing squares: the test asks whether radius / distance * focal length
    // falls below a pixel threshold, and squaring both sides answers the same
    // question without two square roots per instance. This runs over every
    // instance that survives the frustum, so the roots were worth removing.
    const float sizeRatio = minimumPixels / std::max(pixelsPerRadian, 1e-6f);
    const float sizeRatioSquared = sizeRatio * sizeRatio;
    // The same comparison at a coarser threshold, for deciding whether an
    // instance is worth an occlusion test at all.
    const float occlusionRatio = m_occlusionPixels / std::max(pixelsPerRadian, 1e-6f);
    const float occlusionRatioSquared = occlusionRatio * occlusionRatio;
    // The same question asked of a box with a radius given separately, which is
    // how a cell is judged by the largest thing in it rather than by its own
    // size - a cell is ten metres across whatever stands in it.
    const auto tooSmallForRadius = [&eye, sizeRatioSquared](const std::array<float, 6> &box, float radius) {
        const float centre[3] = {0.5f * (box[0] + box[3]), 0.5f * (box[1] + box[4]), 0.5f * (box[2] + box[5])};
        const float distanceSquared = (centre[0] - eye[0]) * (centre[0] - eye[0]) +
                                      (centre[1] - eye[1]) * (centre[1] - eye[1]) +
                                      (centre[2] - eye[2]) * (centre[2] - eye[2]);
        return distanceSquared > radius * radius && radius * radius < sizeRatioSquared * distanceSquared;
    };

    const auto tooSmallOnScreen = [&eye, minimumPixels, sizeRatioSquared](const std::array<float, 6> &box) {
        if (minimumPixels <= 0.0f)
            return false;
        const float radiusSquared = 0.25f * ((box[3] - box[0]) * (box[3] - box[0]) +
                                             (box[4] - box[1]) * (box[4] - box[1]) +
                                             (box[5] - box[2]) * (box[5] - box[2]));
        const float centre[3] = {0.5f * (box[0] + box[3]), 0.5f * (box[1] + box[4]), 0.5f * (box[2] + box[5])};
        const float distanceSquared = (centre[0] - eye[0]) * (centre[0] - eye[0]) +
                                      (centre[1] - eye[1]) * (centre[1] - eye[1]) +
                                      (centre[2] - eye[2]) * (centre[2] - eye[2]);
        return distanceSquared > radiusSquared && radiusSquared < sizeRatioSquared * distanceSquared;
    };

    m_occluded = 0;
    m_occlusionTests = 0;
    // Filled by the occluder block below, which may not run at all.
    float occludersTimed = 0.0f;
    if (useOcclusion)
    {
        G3_ZONE("occluders");
        const auto occluderStart = std::chrono::steady_clock::now();
        struct Timed
        {
            float &into;
            std::chrono::steady_clock::time_point start;
            ~Timed()
            {
                into = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
            }
        } timed{occludersTimed, occluderStart};

        m_occlusion.clear();
        for (const Occluder &occluder : m_occluders)
        {
            // An occluder behind the camera hides nothing, and rasterising it
            // costs the same as one that does.
            const std::array<float, 6> &box = m_batches[occluder.batch].bounds[occluder.instance];
            if (outsideFrustum(box))
                continue;
            m_occlusion.addOccluder(box, viewProjection);
        }
    }

    m_visible.clear();
    m_tooSmall = 0;
    m_rejectedWhole = 0;
    m_outsideView = 0;
    m_wrongLod = 0;
    m_submittedTriangles = 0;
    m_submittedDraws = 0;
    m_testedInstances = 0;

    m_cullPhases.occluders = occludersTimed;

    const auto cellStart = std::chrono::steady_clock::now();
    // Reject whole cells first. From an overview of the map every batch is
    // inside the frustum, so testing them one at a time rejects nothing; a cell
    // takes its hundreds of batches with it. A cell survives if any part of it
    // is in view and its largest instance is big enough to see.
    std::vector<char> skipBatch(m_batches.size(), 0);
    for (const Cell &cell : m_cells)
    {
        if (!outsideFrustum(cell.bounds) &&
            !(minimumPixels > 0.0f && tooSmallForRadius(cell.bounds, cell.largestRadius)))
            continue;
        for (std::size_t index : cell.batches)
            skipBatch[index] = 1;
    }

    m_cullPhases.cells = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() -
                                                                  cellStart).count();

    const auto instanceStart = std::chrono::steady_clock::now();
    // One batch at a time, in any order, on any thread. Nothing in here is
    // shared: the block a batch writes into is its own, its ranges are its
    // own, and the counters are per thread.
    if (!m_pool)
        m_pool = std::make_unique<Pool>();
    m_cullCounts.assign(m_pool->threads(), CullCounts{});
    const unsigned jitter = m_cullJitter;
    const std::uint64_t jitterSeed = device.frameCounter();
    const auto cullBatch = [&](std::size_t batchIndex, unsigned thread) {
        const auto batchStarted = std::chrono::steady_clock::now();
        struct Timed
        {
            CullCounts &into;
            std::chrono::steady_clock::time_point start;
            ~Timed()
            {
                into.seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                ++into.batches;
            }
        } timed{m_cullCounts[thread], batchStarted};

        if (jitter != 0)
        {
            // Deliberately wasteful, and deliberately different for every
            // (batch, thread, frame): the point is that two runs never line up.
            std::uint64_t state = jitterSeed * 0x9E3779B97F4A7C15ull + std::uint64_t(batchIndex) * 0xBF58476D1CE4E5B9ull +
                                  std::uint64_t(thread) * 0x94D049BB133111EBull;
            state ^= state >> 30;
            state *= 0xBF58476D1CE4E5B9ull;
            state ^= state >> 27;
            volatile std::uint64_t sink = 0;
            const std::uint64_t spins = state % (std::uint64_t(jitter) + 1);
            for (std::uint64_t at = 0; at < spins; ++at)
                sink = sink + at;
            (void)sink;
        }

        CullCounts &counts = m_cullCounts[thread];
        Batch &batch = m_batches[batchIndex];
        const std::uint32_t firstInstance = static_cast<std::uint32_t>(batch.instanceBase);
        std::uint32_t visible = 0;
        genome::WorldMatrix *out =
            static_cast<genome::WorldMatrix *>(m_instanceBuffer[device.frameIndex()].mapped) + batch.instanceBase;

        if (skipBatch[batchIndex] ||
            (batch.hasExtent && (outsideFrustum(batch.extent) || tooSmallOnScreen(batch.extent))))
        {
            // Rejected whole - by the cell it sits in, by the frustum, or by
            // being too small. Its own bucket: calling all three "too small"
            // reported a batch behind the camera as too small to see.
            counts.rejectedWhole += batch.transforms.size();
            for (Range &range : batch.ranges)
            {
                range.firstInstance = firstInstance;
                range.instanceCount = 0;
            }
            return;
        }

        counts.tested += batch.transforms.size();
        const float nearSquared = batch.lodNear * batch.lodNear;
        const float farSquared = batch.lodFar * batch.lodFar;

        // Asked once. A batch either has a box for every instance or none at
        // all, but the loop used to re-load the size and compare it six times
        // an instance, once before each test that wants one.
        const std::size_t boxCount = batch.bounds.size();
        for (std::size_t instance = 0; instance < batch.transforms.size(); ++instance)
        {
            const bool hasBox = instance < boxCount;
            if (hasBox && outsideFrustum(batch.bounds[instance]))
            {
                ++counts.outsideView;
                continue;
            }

            // Which detail level draws this one. Measured to the nearest face of
            // the box rather than its centre, so a tall tree does not switch
            // early just because its middle is far away.
            if ((nearSquared > 0.0f || farSquared > 0.0f) && hasBox)
            {
                const std::array<float, 6> &box = batch.bounds[instance];
                float distanceSquared = 0.0f;
                for (int axis = 0; axis < 3; ++axis)
                {
                    const float low = box[axis] - eye[axis];
                    const float high = eye[axis] - box[axis + 3];
                    const float outside = std::max(0.0f, std::max(low, high));
                    distanceSquared += outside * outside;
                }
                if (distanceSquared < nearSquared || (farSquared > 0.0f && distanceSquared >= farSquared))
                {
                    // Drawn by the other detail level, or by the billboard. Two
                    // thirds of a tree's instances leave here, since a kind is
                    // three batches sharing one set of transforms.
                    ++counts.wrongLod;
                    continue;
                }
            }

            if (hasBox && tooSmallOnScreen(batch.bounds[instance]))
            {
                ++counts.tooSmall;
                continue;
            }

            if (useOcclusion && hasBox)
            {
                const std::array<float, 6> &box = batch.bounds[instance];
                const float extent = std::max({box[3] - box[0], box[4] - box[1], box[5] - box[2]});

                // Big enough on screen to be worth asking about. The test is
                // twenty-odd nanoseconds and a small instance costs the card
                // less than that to draw, so below the threshold the question
                // can never pay for itself.
                bool worthAsking = true;
                if (occlusionRatioSquared > 0.0f)
                {
                    const float radius = 0.5f * std::sqrt((box[3] - box[0]) * (box[3] - box[0]) +
                                                          (box[4] - box[1]) * (box[4] - box[1]) +
                                                          (box[5] - box[2]) * (box[5] - box[2]));
                    const float centre[3] = {0.5f * (box[0] + box[3]), 0.5f * (box[1] + box[4]),
                                             0.5f * (box[2] + box[5])};
                    const float distanceSquared = (centre[0] - eye[0]) * (centre[0] - eye[0]) +
                                                  (centre[1] - eye[1]) * (centre[1] - eye[1]) +
                                                  (centre[2] - eye[2]) * (centre[2] - eye[2]);
                    worthAsking = radius * radius >= occlusionRatioSquared * distanceSquared;
                }

                // Occluders are not tested against themselves.
                if (worthAsking && extent < 1000.0f)
                {
                    ++counts.occlusionTests;
                    if (m_occlusion.isOccluded(box, viewProjection))
                    {
                        ++counts.occluded;
                        continue;
                    }
                }
            }

            if (batch.faceCamera && hasBox)
            {
                // A quad standing on the ground where the tree stands, as wide
                // and as tall as the tree was, turned to face the camera about
                // the vertical axis only - a billboard that leans looks wrong
                // the moment the camera rises.
                const std::array<float, 6> &box = batch.bounds[instance];
                const float centreX = 0.5f * (box[0] + box[3]);
                const float centreZ = 0.5f * (box[2] + box[5]);
                const float width = std::max(box[3] - box[0], box[5] - box[2]);
                const float height = box[4] - box[1];

                float toEyeX = eye[0] - centreX;
                float toEyeZ = eye[2] - centreZ;
                const float length = std::sqrt(toEyeX * toEyeX + toEyeZ * toEyeZ);
                if (length > 1e-3f)
                {
                    toEyeX /= length;
                    toEyeZ /= length;
                }
                else
                {
                    toEyeX = 0.0f;
                    toEyeZ = 1.0f;
                }

                // Right is the horizontal perpendicular to the view direction.
                genome::WorldMatrix m{};
                m[0] = -toEyeZ * width;
                m[1] = 0.0f;
                m[2] = toEyeX * width;
                m[4] = 0.0f;
                m[5] = height;
                m[6] = 0.0f;
                m[8] = toEyeX;
                m[9] = 0.0f;
                m[10] = toEyeZ;
                m[12] = centreX;
                m[13] = box[1];
                m[14] = centreZ;
                m[15] = 1.0f;
                out[visible++] = m;
                continue;
            }

            out[visible++] = batch.transforms[instance];
        }

        counts.visible += visible;
        for (Range &range : batch.ranges)
        {
            range.firstInstance = firstInstance;
            range.instanceCount = visible;
            if (visible != 0)
            {
                ++counts.draws;
                counts.triangles += std::size_t(range.indexCount / 3) * visible;
            }
        }
    };

    // Eight at a time, because a median batch of six instances would
    // otherwise be one atomic increment each.
    // One batch at a time. With the heaviest first, a run of eight would hand
    // one thread the eight biggest together, which is the pile-up being undone.
    const auto cullInOrder = [&](std::size_t at, unsigned thread) { cullBatch(m_cullOrder[at], thread); };
    m_pool->forEach(m_cullOrder.size(), m_cullGrain, cullInOrder);

    std::size_t visibleTotal = 0;
    for (const CullCounts &counts : m_cullCounts)
    {
        m_rejectedWhole += counts.rejectedWhole;
        m_outsideView += counts.outsideView;
        m_wrongLod += counts.wrongLod;
        visibleTotal += counts.visible;
        m_tooSmall += counts.tooSmall;
        m_occluded += counts.occluded;
        m_testedInstances += counts.tested;
        m_occlusionTests += counts.occlusionTests;
        m_submittedDraws += counts.draws;
        m_submittedTriangles += counts.triangles;
    }

    m_cullPhases.instances = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() -
                                                                      instanceStart).count();

    const auto lightStart = std::chrono::steady_clock::now();
    // The lights that reach this frame: nearest first, and only those whose own
    // range still covers the camera's surroundings. 588 in the world, at most
    // sixteen in a frame.
    {
        G3_ZONE("pick lights");
        std::vector<std::pair<float, const genome::PointLight *>> near;
        near.reserve(m_lights.size());
        for (const genome::PointLight &light : m_lights)
        {
            const float dx = light.position[0] - eye[0];
            const float dy = light.position[1] - eye[1];
            const float dz = light.position[2] - eye[2];
            const float distance = dx * dx + dy * dy + dz * dz;
            // Beyond a few times its own range a light contributes nothing that
            // survives rounding.
            const float reach = light.range * 6.0f;
            if (distance < reach * reach)
                near.push_back({distance, &light});
        }
        std::sort(near.begin(), near.end(),
                  [](const auto &a, const auto &b) { return a.first < b.first; });
        if (near.size() > c_MaxFrameLights)
            near.resize(c_MaxFrameLights);
        m_litLights = near.size();

        float *out = static_cast<float *>(m_lightBuffer[device.frameIndex()].mapped);
        out[0] = float(near.size());
        out[1] = out[2] = out[3] = 0.0f;
        // The shader declares two arrays, not an array of pairs, and std140 lays
        // them out one after the other: the count, then sixteen position-and-
        // range vectors, then sixteen colours. Writing them interleaved made the
        // shader read positions as colours, and a colour of 58700 turns the
        // whole picture white.
        float *positions = out + 4;
        float *colours = out + 4 + 4 * c_MaxFrameLights;
        for (std::size_t index = 0; index < near.size(); ++index)
        {
            const genome::PointLight &light = *near[index].second;
            positions[index * 4 + 0] = light.position[0];
            positions[index * 4 + 1] = light.position[1];
            positions[index * 4 + 2] = light.position[2];
            positions[index * 4 + 3] = light.range;
            colours[index * 4 + 0] = light.colour[0];
            colours[index * 4 + 1] = light.colour[1];
            colours[index * 4 + 2] = light.colour[2];
            colours[index * 4 + 3] = 0.0f;
        }
    }

    m_cullPhases.lights = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() -
                                                                   lightStart).count();
    m_visibleInstances = visibleTotal;

    // Every instance loaded left the loop through exactly one exit, so the
    // buckets have to come to the total. If they do not, the parallel merge
    // lost or doubled some - a stronger and much earlier signal than a
    // screenshot that happens to differ. Checked after visibleTotal is in
    // place: reading it before that line compared this frame's rejections
    // against last frame's survivors, which is how this check first caught
    // itself.
    const std::size_t accounted =
        m_visibleInstances + m_rejectedWhole + m_tooSmall + m_occluded + m_outsideView + m_wrongLod;
    m_unaccounted = std::ptrdiff_t(m_instanceCount) - std::ptrdiff_t(accounted);
    assert(m_unaccounted == 0 && "the cull lost instances: a bucket is missing or the merge dropped a thread");
    if (m_unaccounted != 0 && !m_toldAboutLostInstances)
    {
        // assert is compiled out under NDEBUG, which is what everything here is
        // measured in, so the check that matters most has to speak anyway.
        m_toldAboutLostInstances = true;
        std::printf("the cull did not account for %td of %zu instances; a bucket is missing or the merge is wrong\n",
                    m_unaccounted, m_instanceCount);
    }

    {
        // Its own scope: a zone names a local, so two in one scope collide.
        G3_ZONE("upload instances");
        // Nothing to copy: the cull wrote straight into the mapped buffer,
        // each batch into its own block.
    }
}

void WorldRenderer::startProfiling(Device &device)
{
    if (!m_gpu)
        m_gpu = gpuContextCreate(device.physicalDevice(), device.device(), device.queue(), device.commandBuffer());
}

void WorldRenderer::collectProfiling(Device &device)
{
    gpuCollect(m_gpu, device.commandBuffer());
}

void WorldRenderer::stopProfiling()
{
    gpuContextDestroy(m_gpu);
}

void WorldRenderer::buildGrid()
{
    // The game streams on a 10000-unit grid and its sector files are named for
    // it, so a cell of the same size groups what was authored together.
    constexpr float c_CellSize = 10000.0f;

    m_cells.clear();
    m_looseBatches.clear();

    // Keyed by place and by size. One house in a cell would otherwise keep every
    // blade of grass around it alive, because the cell is judged by its largest
    // instance - so things of a size are grouped together and a cell of grass is
    // rejected without the house having a say.
    std::map<std::tuple<int, int, int>, std::size_t> cellAt;
    for (std::size_t index = 0; index < m_batches.size(); ++index)
    {
        const Batch &batch = m_batches[index];
        if (!batch.hasExtent)
        {
            m_looseBatches.push_back(index);
            continue;
        }

        // A batch wider than a cell would make its cell useless, so it stays
        // outside the grid and is tested on its own.
        const float width = batch.extent[3] - batch.extent[0];
        const float depth = batch.extent[5] - batch.extent[2];
        if (width > c_CellSize || depth > c_CellSize)
        {
            m_looseBatches.push_back(index);
            continue;
        }

        float largest = 0.0f;
        for (const std::array<float, 6> &box : batch.bounds)
        {
            const float radius = 0.5f * std::sqrt((box[3] - box[0]) * (box[3] - box[0]) +
                                                  (box[4] - box[1]) * (box[4] - box[1]) +
                                                  (box[5] - box[2]) * (box[5] - box[2]));
            largest = std::max(largest, radius);
        }

        // Octaves of size: everything within a factor of two shares a bucket.
        const int sizeClass = largest > 1.0f ? std::min(15, int(std::log2(largest))) : 0;
        const std::tuple<int, int, int> key{int(std::floor(0.5f * (batch.extent[0] + batch.extent[3]) / c_CellSize)),
                                            int(std::floor(0.5f * (batch.extent[2] + batch.extent[5]) / c_CellSize)),
                                            sizeClass};
        auto found = cellAt.find(key);
        if (found == cellAt.end())
        {
            found = cellAt.emplace(key, m_cells.size()).first;
            Cell cell;
            cell.bounds = batch.extent;
            m_cells.push_back(std::move(cell));
        }

        Cell &cell = m_cells[found->second];
        cell.batches.push_back(index);
        for (int axis = 0; axis < 3; ++axis)
        {
            cell.bounds[axis] = std::min(cell.bounds[axis], batch.extent[axis]);
            cell.bounds[axis + 3] = std::max(cell.bounds[axis + 3], batch.extent[axis + 3]);
        }
        cell.largestRadius = std::max(cell.largestRadius, largest);
    }

    if (!m_reportedGrid)
        std::printf("%zu grid cells hold %zu batches, %zu tested on their own\n", m_cells.size(),
                    m_batches.size() - m_looseBatches.size(), m_looseBatches.size());
    m_reportedGrid = true;
}

const std::array<float, 6> *WorldRenderer::batchExtent(std::size_t batch) const
{
    if (batch >= m_batches.size() || !m_batches[batch].hasExtent)
        return nullptr;
    return &m_batches[batch].extent;
}

void WorldRenderer::prepareAll(Device &device)
{
    m_visible.clear();
    for (Batch &batch : m_batches)
    {
        const std::uint32_t firstInstance = static_cast<std::uint32_t>(m_visible.size());
        for (const genome::WorldMatrix &transform : batch.transforms)
            m_visible.push_back(transform);
        for (Range &range : batch.ranges)
        {
            range.firstInstance = firstInstance;
            range.instanceCount = static_cast<std::uint32_t>(batch.transforms.size());
        }
    }
    if (!m_visible.empty())
        std::memcpy(m_instanceBuffer[device.frameIndex()].mapped, m_visible.data(),
                    sizeof(genome::WorldMatrix) * m_visible.size());
    m_visibleInstances = m_visible.size();
}

void WorldRenderer::drawBatch(Device &device, std::size_t batch, const std::array<float, 16> &viewProjection,
                              VkCommandBuffer command)
{
    if (batch >= m_batches.size())
        return;

    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    // The pipeline uses set 1 whatever it is drawing, so the bake binds it too -
    // with whatever lights the last cull left there, which do not reach a
    // billboard drawn under an orthographic camera anyway.
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 1, 1,
                            &m_lightSet[device.frameIndex()], 0, nullptr);

    // The billboard bake wants the flat normal: the impostor is a card.
    PushConstants push{viewProjection, {0.0f, 1.0f, 0.0f, 0.0f}, 0.0f, 0.0f};
    vkCmdPushConstants(command, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                       &push);

    const VkDeviceSize offsets[2] = {0, 0};
    const VkBuffer buffers[2] = {m_vertices.handle(), m_instanceBuffer[device.frameIndex()].handle};
    vkCmdBindVertexBuffers(command, 0, 2, buffers, offsets);
    vkCmdBindIndexBuffer(command, m_indices.handle(), 0, VK_INDEX_TYPE_UINT32);

    for (const Range &range : m_batches[batch].ranges)
    {
        if (range.instanceCount == 0)
            continue;

        const float alphaTest = range.alphaTested ? 1.0f : 0.0f;
        vkCmdPushConstants(command, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           offsetof(PushConstants, alphaTested), sizeof(alphaTest), &alphaTest);
        if (range.descriptor != VK_NULL_HANDLE)
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &range.descriptor, 0,
                                    nullptr);
        vkCmdDrawIndexed(command, range.indexCount, range.instanceCount, range.firstIndex, range.vertexOffset,
                         range.firstInstance);
    }
}

void WorldRenderer::draw(Device &device, const std::array<float, 16> &viewProjection,
                         const std::array<float, 4> &light)
{
    G3_ZONE("record draws");

    VkCommandBuffer command = device.commandBuffer();
    G3_GPU_ZONE(m_gpu, command, "world");
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    PushConstants push{viewProjection, light, 0.0f, m_normalStrength};
    vkCmdPushConstants(command, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                       &push);

    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 1, 1,
                            &m_lightSet[device.frameIndex()], 0, nullptr);

    const VkDeviceSize offsets[2] = {0, 0};
    const VkBuffer buffers[2] = {m_vertices.handle(), m_instanceBuffer[device.frameIndex()].handle};
    vkCmdBindVertexBuffers(command, 0, 2, buffers, offsets);
    vkCmdBindIndexBuffer(command, m_indices.handle(), 0, VK_INDEX_TYPE_UINT32);

    // One draw per range: tiles sharing a regional material share a descriptor,
    // so this is a handful of texture binds rather than one per tile.
    VkDescriptorSet bound = VK_NULL_HANDLE;
    float boundAlphaTest = -1.0f;
    for (const Batch &batch : m_batches)
        for (const Range &range : batch.ranges)
    {
        if (range.instanceCount == 0)
            continue;

        const float wanted = range.alphaTested ? 1.0f : 0.0f;
        if (wanted != boundAlphaTest)
        {
            // The declared range covers both stages, so a push into it has to
            // name both - even though only the fragment shader reads this byte.
            vkCmdPushConstants(command, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               offsetof(PushConstants, alphaTested), sizeof(wanted), &wanted);
            boundAlphaTest = wanted;
        }

        if (range.descriptor != bound)
        {
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &range.descriptor, 0,
                                    nullptr);
            bound = range.descriptor;
        }
        vkCmdDrawIndexed(command, range.indexCount, range.instanceCount, range.firstIndex,
                         range.vertexOffset, range.firstInstance);
    }
}

void WorldRenderer::destroy(Device &device)
{
    retireReleases(device, m_frameCounter + Device::c_FramesInFlight + 2);
    for (auto &[image, entry] : m_textureOf)
        destroyTexture(device, entry.texture);
    m_textureOf.clear();
    m_setOf.clear();
    destroyTexture(device, m_flatNormal);
    destroyTexture(device, m_white);
    for (std::uint32_t frame = 0; frame < Device::c_FramesInFlight; ++frame)
        device.destroyBuffer(m_lightBuffer[frame]);
    m_coordArena.destroy(device);
    m_lightmapArena.destroy(device);
    m_incidentArena.destroy(device);
    m_uploader.destroy(device);
    destroyTexture(device, m_lightmapTexture);
    if (m_lightPool)
        vkDestroyDescriptorPool(device.device(), m_lightPool, nullptr);
    if (m_lightLayout)
        vkDestroyDescriptorSetLayout(device.device(), m_lightLayout, nullptr);
    m_lightPool = VK_NULL_HANDLE;
    m_lightLayout = VK_NULL_HANDLE;

    if (m_descriptorPool)
        vkDestroyDescriptorPool(device.device(), m_descriptorPool, nullptr);
    if (m_descriptorLayout)
        vkDestroyDescriptorSetLayout(device.device(), m_descriptorLayout, nullptr);

    m_vertices.destroy(device);
    m_indices.destroy(device);
    for (Buffer &buffer : m_instanceBuffer)
        device.destroyBuffer(buffer);
    if (m_pipeline)
        vkDestroyPipeline(device.device(), m_pipeline, nullptr);
    if (m_layout)
        vkDestroyPipelineLayout(device.device(), m_layout, nullptr);
    m_pipeline = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
}

} // namespace render
