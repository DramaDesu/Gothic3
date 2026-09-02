#include "world_renderer.h"

#include "profile.h"

#include <cstdio>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
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
    float alphaTested = 0.0f; // per draw, not per frame
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

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    vkCreateDescriptorSetLayout(device.device(), &layoutInfo, nullptr, &m_descriptorLayout);

    // Sets are handed out as textures turn up rather than counted first, so the
    // pool is sized by the budget.
    const std::uint32_t setCount = budget.textures + 1;
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, setCount};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = setCount;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    vkCreateDescriptorPool(device.device(), &poolInfo, nullptr, &m_descriptorPool);

    m_whiteSet = descriptorFor(device, nullptr, error);
    if (!m_whiteSet)
        return false;

    return createPipeline(device, error);
}

VkDescriptorSet WorldRenderer::descriptorFor(Device &device, const genome::Image *image, std::string *error)
{
    if (image)
    {
        const auto existing = m_textureSets.find(image);
        if (existing != m_textureSets.end())
            return existing->second;
    }
    else if (m_whiteSet)
        return m_whiteSet;

    Texture texture = m_white;
    if (image && !createTexture(device, *image, true, texture, error))
        return VK_NULL_HANDLE;

    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = m_descriptorPool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &m_descriptorLayout;
    if (vkAllocateDescriptorSets(device.device(), &allocate, &set) != VK_SUCCESS)
    {
        if (error)
            *error = "the descriptor pool is out of sets; raise Budget::textures";
        return VK_NULL_HANDLE;
    }

    VkDescriptorImageInfo imageInfo{texture.sampler, texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device.device(), 1, &write, 0, nullptr);

    if (image)
    {
        m_textures.push_back(texture);
        m_textureSets.emplace(image, set);
    }
    return set;
}

// Puts a mesh in the arenas, or finds the copy already there. The element table
// is rebased once here, so every batch drawing this mesh reuses the same draw
// parameters rather than working them out again.
bool WorldRenderer::placeMesh(Device &device, const genome::Mesh &mesh, MeshGeometry *&out, std::string *error)
{
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

    if (!m_uploader.write(device, m_vertices, geometry.vertexOffset, vertices.data(), vertices.size(), error))
        return false;
    if (!m_uploader.write(device, m_indices, geometry.indexOffset, indices.data(), indices.size(), error))
        return false;

    m_vertexCount += vertexCount;
    m_indexCount += indexCount;
    out = &m_geometry.emplace(&mesh, std::move(geometry)).first->second;
    return true;
}

bool WorldRenderer::addBatches(Device &device, const std::vector<MeshInstances> &batches, std::string *error)
{
    std::size_t litInstances = 0;

    for (const MeshInstances &batch : batches)
    {
        if (!batch.mesh || batch.transforms.empty())
            continue;

        MeshGeometry *geometry = nullptr;
        if (!placeMesh(device, *batch.mesh, geometry, error))
            return false;
        if (!geometry)
            continue;

        Batch kept;
        kept.mesh = batch.mesh;
        kept.transforms = batch.transforms;

        // Where this mesh's vertices begin in the arena. The shader indexes its
        // lighting with gl_VertexIndex, which counts from the start of the
        // buffer rather than from the start of the mesh.
        const std::int32_t meshFirstVertex = std::int32_t(geometry->vertexOffset);

        // Two unused corners of the transform carry it: the first row's w holds
        // the bias, which may be negative, and the second row's w says whether
        // there is any lighting at all - a sentinel in the bias would be
        // indistinguishable from a legitimate negative one.
        for (std::size_t index = 0; index < kept.transforms.size(); ++index)
        {
            const bool lit = index < batch.lightmapBase.size() && batch.lightmapBase[index] >= 0;
            kept.transforms[index][3] = lit ? float(batch.lightmapBase[index] - meshFirstVertex) : 0.0f;
            kept.transforms[index][7] = lit ? 1.0f : 0.0f;
            litInstances += lit ? 1 : 0;
        }
        kept.bounds = batch.bounds;

        for (const std::array<float, 6> &box : kept.bounds)
        {
            if (!kept.hasExtent)
            {
                kept.extent = box;
                kept.hasExtent = true;
                continue;
            }
            for (int axis = 0; axis < 3; ++axis)
            {
                kept.extent[axis] = std::min(kept.extent[axis], box[axis]);
                kept.extent[axis + 3] = std::max(kept.extent[axis + 3], box[axis + 3]);
            }
        }
        // Only useful when every instance has bounds; otherwise some are drawn
        // unconditionally and the batch cannot be rejected as a whole.
        kept.hasExtent = kept.hasExtent && kept.bounds.size() == kept.transforms.size();
        kept.occludes = batch.occludes;
        kept.lodNear = batch.lodNear;
        kept.lodFar = batch.lodFar;
        kept.faceCamera = batch.faceCamera;

        // The world's extent, for whoever wants to point a camera at it. Where
        // the instances brought their own bounds those are exact; where they did
        // not, the mesh's own box is taken through each transform by its eight
        // corners, which is a superset of the vertices and vastly cheaper than
        // walking every vertex through every transform.
        if (kept.hasExtent)
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                m_boundsMin[axis] = std::min(m_boundsMin[axis], kept.extent[axis]);
                m_boundsMax[axis] = std::max(m_boundsMax[axis], kept.extent[axis + 3]);
            }
        }
        else
        {
            std::array<float, 6> local{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                                       std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest(),
                                       std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
            for (const genome::MeshElement &element : batch.mesh->elements)
                for (const std::array<float, 3> &position : element.positions)
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        local[axis] = std::min(local[axis], position[axis]);
                        local[axis + 3] = std::max(local[axis + 3], position[axis]);
                    }

            for (const genome::WorldMatrix &m : kept.transforms)
                for (int corner = 0; corner < 8; ++corner)
                {
                    const std::array<float, 3> v{local[(corner & 1) ? 3 : 0], local[(corner & 2) ? 4 : 1],
                                                 local[(corner & 4) ? 5 : 2]};
                    const std::array<float, 3> world{v[0] * m[0] + v[1] * m[4] + v[2] * m[8] + m[12],
                                                     v[0] * m[1] + v[1] * m[5] + v[2] * m[9] + m[13],
                                                     v[0] * m[2] + v[1] * m[6] + v[2] * m[10] + m[14]};
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        m_boundsMin[axis] = std::min(m_boundsMin[axis], world[axis]);
                        m_boundsMax[axis] = std::max(m_boundsMax[axis], world[axis]);
                    }
                }
        }

        std::size_t elementIndex = 0, drawable = 0;
        for (const genome::MeshElement &element : batch.mesh->elements)
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
            range.firstInstance = 0;
            range.instanceCount = std::uint32_t(batch.transforms.size());
            range.alphaTested = elementIndex < batch.alphaTested.size() && batch.alphaTested[elementIndex] != 0;

            const genome::Image *image =
                elementIndex < batch.textures.size() ? batch.textures[elementIndex] : nullptr;
            range.descriptor = descriptorFor(device, image, error);
            if (!range.descriptor)
                return false;

            kept.ranges.push_back(range);
            ++m_rangeCount;
            ++elementIndex;
        }

        // Anything spanning more than about ten metres is worth rasterising as
        // an occluder: houses, cliffs and the landscape itself, not barrels.
        const std::size_t batchIndex = m_batches.size();
        for (std::size_t instance = 0; instance < kept.bounds.size(); ++instance)
        {
            const std::array<float, 6> &box = kept.bounds[instance];
            const float extent = std::max({box[3] - box[0], box[4] - box[1], box[5] - box[2]});
            if (extent < 1000.0f)
                continue;
            if (kept.occludes)
                m_occluders.push_back({batchIndex, instance});
            else
                ++m_foliageSkipped;
        }

        m_instanceCount += kept.transforms.size();
        m_batches.push_back(std::move(kept));
    }

    if (!m_uploader.flush(device, error))
        return false;

    if (m_instanceCount > m_budget.instances)
    {
        if (error)
            *error = "more instances than the buffer holds; raise Budget::instances";
        return false;
    }

    if (litInstances != 0)
        std::printf("%zu instances carry baked lighting into the shader\n", litInstances);
    buildGrid();
    return true;
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
    if (!addBatches(device, batches, error))
        return false;

    if (m_vertexCount == 0 || m_instanceCount == 0)
    {
        if (error)
            *error = "no geometry to draw";
        return false;
    }
    std::printf("%zu occluders, %zu large enough but foliage\n", m_occluders.size(), m_foliageSkipped);
    return true;
}

void WorldRenderer::reportArenas() const
{
    std::printf("arenas: %.0f%% of %zuM vertices, %.0f%% of %zuM indices, %.0f%% of %zuM lit vertices, "
                "%zu textures, %zu staging submits\n",
                100.0 * double(m_vertices.highWater()) / double(m_vertices.capacity()), m_vertices.capacity() >> 20,
                100.0 * double(m_indices.highWater()) / double(m_indices.capacity()), m_indices.capacity() >> 20,
                100.0 * double(m_lightmapArena.highWater()) / double(m_lightmapArena.capacity()),
                m_lightmapArena.capacity() >> 20, m_textureSets.size(), m_uploader.submits());
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
    if (m_lightmapColours.empty())
        m_lightmapColours.push_back(0xFFFFFFFF);
    if (m_lightmapIncident.empty())
        m_lightmapIncident.assign(3, 0.0f);
    if (m_lightmapCoords.empty())
        m_lightmapCoords.assign(2, -1.0f);

    struct Upload
    {
        GpuArena *arena;
        const void *data;
        std::size_t count;
        const char *what;
    };
    const Upload uploads[3] = {{&m_lightmapArena, m_lightmapColours.data(), m_lightmapColours.size(), "colours"},
                               {&m_incidentArena, m_lightmapIncident.data(), m_lightmapIncident.size(), "directions"},
                               {&m_coordArena, m_lightmapCoords.data(), m_lightmapCoords.size(), "patch coordinates"}};
    for (const Upload &upload : uploads)
    {
        // Allocated rather than assumed to start at zero: the base a transform
        // carries is an index into the arena, not into the array it came from.
        const std::size_t base = upload.arena->allocate(upload.count);
        if (base == GpuArena::npos)
        {
            if (error)
                *error = std::string("the lighting arena has no room for the baked ") + upload.what +
                         "; raise Budget::lightVertices";
            return false;
        }
        if (base != 0)
        {
            if (error)
                *error = "the baked lighting was not the first thing into its arena";
            return false;
        }
        if (!m_uploader.write(device, *upload.arena, base, upload.data, upload.count, error))
            return false;
    }
    if (!m_uploader.flush(device, error))
        return false;

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
    if (useOcclusion)
    {
        G3_ZONE("occluders");

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
    m_submittedTriangles = 0;
    m_submittedDraws = 0;
    m_testedInstances = 0;

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

    for (std::size_t batchIndex = 0; batchIndex < m_batches.size(); ++batchIndex)
    {
        Batch &batch = m_batches[batchIndex];
        const std::uint32_t firstInstance = static_cast<std::uint32_t>(m_visible.size());
        std::uint32_t visible = 0;

        if (skipBatch[batchIndex] ||
            (batch.hasExtent && (outsideFrustum(batch.extent) || tooSmallOnScreen(batch.extent))))
        {
            // Rejected whole. Its instances still have to be accounted for, or
            // the counts stop adding up to what was loaded.
            m_tooSmall += batch.transforms.size();
            for (Range &range : batch.ranges)
            {
                range.firstInstance = firstInstance;
                range.instanceCount = 0;
            }
            continue;
        }

        m_testedInstances += batch.transforms.size();
        const float nearSquared = batch.lodNear * batch.lodNear;
        const float farSquared = batch.lodFar * batch.lodFar;

        for (std::size_t instance = 0; instance < batch.transforms.size(); ++instance)
        {
            if (instance < batch.bounds.size() && outsideFrustum(batch.bounds[instance]))
                continue;

            // Which detail level draws this one. Measured to the nearest face of
            // the box rather than its centre, so a tall tree does not switch
            // early just because its middle is far away.
            if ((nearSquared > 0.0f || farSquared > 0.0f) && instance < batch.bounds.size())
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
                if (distanceSquared < nearSquared)
                    continue;
                if (farSquared > 0.0f && distanceSquared >= farSquared)
                    continue;
            }

            if (instance < batch.bounds.size() && tooSmallOnScreen(batch.bounds[instance]))
            {
                ++m_tooSmall;
                continue;
            }

            if (useOcclusion && instance < batch.bounds.size())
            {
                const std::array<float, 6> &box = batch.bounds[instance];
                const float extent = std::max({box[3] - box[0], box[4] - box[1], box[5] - box[2]});
                // Occluders are not tested against themselves.
                if (extent < 1000.0f && m_occlusion.isOccluded(box, viewProjection))
                {
                    ++m_occluded;
                    continue;
                }
            }

            if (batch.faceCamera && instance < batch.bounds.size())
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
                m_visible.push_back(m);
                ++visible;
                continue;
            }

            m_visible.push_back(batch.transforms[instance]);
            ++visible;
        }

        for (Range &range : batch.ranges)
        {
            range.firstInstance = firstInstance;
            range.instanceCount = visible;
            if (visible != 0)
            {
                ++m_submittedDraws;
                m_submittedTriangles += std::size_t(range.indexCount / 3) * visible;
            }
        }
    }

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

    m_visibleInstances = m_visible.size();

    {
        // Its own scope: a zone names a local, so two in one scope collide.
        G3_ZONE("upload instances");
        if (!m_visible.empty())
            std::memcpy(m_instanceBuffer[device.frameIndex()].mapped, m_visible.data(),
                        sizeof(genome::WorldMatrix) * m_visible.size());
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

    std::printf("%zu grid cells hold %zu batches, %zu tested on their own\n", m_cells.size(),
                m_batches.size() - m_looseBatches.size(), m_looseBatches.size());
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

    PushConstants push{viewProjection, {0.0f, 1.0f, 0.0f, 0.0f}};
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

    PushConstants push{viewProjection, light};
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
    for (Texture &texture : m_textures)
        destroyTexture(device, texture);
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
