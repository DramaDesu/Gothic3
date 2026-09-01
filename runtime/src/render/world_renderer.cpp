#include "world_renderer.h"

#include "profile.h"

#include <cstdio>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>

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

bool WorldRenderer::create(Device &device, const std::vector<MeshInstances> &batches, std::string *error)
{
    std::vector<WorldVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<genome::WorldMatrix> instances;

    m_boundsMin = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max()};
    m_boundsMax = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                   std::numeric_limits<float>::lowest()};

    std::map<const genome::Image *, std::size_t> uploaded;
    std::vector<std::size_t> textureForRange;

    for (const MeshInstances &batch : batches)
    {
        if (!batch.mesh || batch.transforms.empty())
            continue;

        const std::uint32_t firstInstance = static_cast<std::uint32_t>(instances.size());
        instances.insert(instances.end(), batch.transforms.begin(), batch.transforms.end());

        Batch kept;
        kept.transforms = batch.transforms;
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

        std::size_t elementIndex = 0;
        for (const genome::MeshElement &element : batch.mesh->elements)
        {
            if (element.positions.empty() || element.indices.empty())
            {
                ++elementIndex;
                continue;
            }

            Range range;
            range.firstIndex = static_cast<std::uint32_t>(indices.size());
            range.indexCount = static_cast<std::uint32_t>(element.indices.size());
            range.vertexOffset = static_cast<std::int32_t>(vertices.size());
            range.firstInstance = firstInstance;
            range.instanceCount = static_cast<std::uint32_t>(batch.transforms.size());
            range.alphaTested = elementIndex < batch.alphaTested.size() && batch.alphaTested[elementIndex] != 0;
            kept.ranges.push_back(m_ranges.size());
            m_ranges.push_back(range);

            const genome::Image *image =
                elementIndex < batch.textures.size() ? batch.textures[elementIndex] : nullptr;
            std::size_t texture = std::size_t(-1);
            if (image)
            {
                const auto existing = uploaded.find(image);
                if (existing != uploaded.end())
                    texture = existing->second;
                else
                {
                    Texture created;
                    if (!createTexture(device, *image, true, created, error))
                        return false;
                    texture = m_textures.size();
                    uploaded.emplace(image, texture);
                    m_textures.push_back(created);
                }
            }
            textureForRange.push_back(texture);

            // Indices stay local to their mesh; vertexOffset does the rebasing at
            // draw time, so a mesh is stored once however often it is placed.
            indices.insert(indices.end(), element.indices.begin(), element.indices.end());

            for (std::size_t index = 0; index < element.positions.size(); ++index)
            {
                WorldVertex vertex{};
                vertex.position = element.positions[index];
                vertex.normal = index < element.normals.size() ? element.normals[index]
                                                               : std::array<float, 3>{0.0f, 1.0f, 0.0f};
                vertex.texCoord = index < element.texCoords.size() ? element.texCoords[index]
                                                                   : std::array<float, 2>{0.0f, 0.0f};
                vertices.push_back(vertex);

                // Vertices are in object space now, so the bounds have to be
                // taken through every transform the mesh is placed with.
                for (const genome::WorldMatrix &m : batch.transforms)
                {
                    const std::array<float, 3> &v = vertex.position;
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
            ++elementIndex;
        }
        m_batches.push_back(std::move(kept));
    }

    // Anything spanning more than about ten metres is worth rasterising as an
    // occluder: houses, cliffs and the landscape itself, not barrels.
    std::size_t foliageSkipped = 0;
    for (std::size_t batch = 0; batch < m_batches.size(); ++batch)
    {
        const Batch &current = m_batches[batch];
        for (std::size_t instance = 0; instance < current.bounds.size(); ++instance)
        {
            const std::array<float, 6> &box = current.bounds[instance];
            const float extent = std::max({box[3] - box[0], box[4] - box[1], box[5] - box[2]});
            if (extent < 1000.0f)
                continue;
            if (current.occludes)
                m_occluders.push_back({batch, instance});
            else
                ++foliageSkipped;
        }
    }
    std::printf("%zu occluders, %zu large enough but foliage\n", m_occluders.size(), foliageSkipped);

    if (vertices.empty() || instances.empty())
    {
        if (error)
            *error = "no geometry to draw";
        return false;
    }

    m_vertexCount = vertices.size();
    m_indexCount = indices.size();
    m_instanceCount = instances.size();

    m_vertexBuffer = device.createBuffer(sizeof(WorldVertex) * vertices.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                         true, error);
    if (!m_vertexBuffer.handle)
        return false;
    std::memcpy(m_vertexBuffer.mapped, vertices.data(), sizeof(WorldVertex) * vertices.size());

    m_indexBuffer =
        device.createBuffer(sizeof(std::uint32_t) * indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true, error);
    if (!m_indexBuffer.handle)
        return false;
    std::memcpy(m_indexBuffer.mapped, indices.data(), sizeof(std::uint32_t) * indices.size());

    // One buffer per frame in flight: culling rewrites it every frame.
    for (Buffer &buffer : m_instanceBuffer)
    {
        buffer = device.createBuffer(sizeof(genome::WorldMatrix) * instances.size(),
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true, error);
        if (!buffer.handle)
            return false;
        std::memcpy(buffer.mapped, instances.data(), sizeof(genome::WorldMatrix) * instances.size());
    }
    m_visible.reserve(instances.size());

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

    const std::uint32_t setCount = static_cast<std::uint32_t>(m_textures.size() + 1);
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, setCount};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = setCount;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    vkCreateDescriptorPool(device.device(), &poolInfo, nullptr, &m_descriptorPool);

    std::vector<VkDescriptorSet> sets(setCount, VK_NULL_HANDLE);
    for (std::uint32_t index = 0; index < setCount; ++index)
    {
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = m_descriptorPool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &m_descriptorLayout;
        vkAllocateDescriptorSets(device.device(), &allocate, &sets[index]);

        const Texture &texture = index < m_textures.size() ? m_textures[index] : m_white;
        VkDescriptorImageInfo imageInfo{texture.sampler, texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = sets[index];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device.device(), 1, &write, 0, nullptr);
    }

    for (std::size_t range = 0; range < m_ranges.size(); ++range)
    {
        const std::size_t texture = textureForRange[range];
        m_ranges[range].descriptor = sets[texture == std::size_t(-1) ? m_textures.size() : texture];
    }

    return createPipeline(device, error);
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

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_descriptorLayout;
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

    m_occluded = 0;
    if (useOcclusion)
    {
        G3_ZONE("occluders");

        m_occlusion.clear();
        for (const Occluder &occluder : m_occluders)
            m_occlusion.addOccluder(m_batches[occluder.batch].bounds[occluder.instance], viewProjection);
    }

    m_visible.clear();
    m_tooSmall = 0;
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

    for (Batch &batch : m_batches)
    {
        const std::uint32_t firstInstance = static_cast<std::uint32_t>(m_visible.size());
        std::uint32_t visible = 0;

        if (batch.hasExtent && (outsideFrustum(batch.extent) || tooSmallOnScreen(batch.extent)))
        {
            // Rejected whole. Its instances still have to be accounted for, or
            // the counts stop adding up to what was loaded.
            m_tooSmall += batch.transforms.size();
            for (std::size_t range : batch.ranges)
            {
                m_ranges[range].firstInstance = firstInstance;
                m_ranges[range].instanceCount = 0;
            }
            continue;
        }

        for (std::size_t instance = 0; instance < batch.transforms.size(); ++instance)
        {
            if (instance < batch.bounds.size() && outsideFrustum(batch.bounds[instance]))
                continue;

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

            m_visible.push_back(batch.transforms[instance]);
            ++visible;
        }

        for (std::size_t range : batch.ranges)
        {
            m_ranges[range].firstInstance = firstInstance;
            m_ranges[range].instanceCount = visible;
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

    const VkDeviceSize offsets[2] = {0, 0};
    const VkBuffer buffers[2] = {m_vertexBuffer.handle, m_instanceBuffer[device.frameIndex()].handle};
    vkCmdBindVertexBuffers(command, 0, 2, buffers, offsets);
    vkCmdBindIndexBuffer(command, m_indexBuffer.handle, 0, VK_INDEX_TYPE_UINT32);

    // One draw per range: tiles sharing a regional material share a descriptor,
    // so this is a handful of texture binds rather than one per tile.
    VkDescriptorSet bound = VK_NULL_HANDLE;
    float boundAlphaTest = -1.0f;
    for (const Range &range : m_ranges)
    {
        if (range.instanceCount == 0)
            continue;

        const float wanted = range.alphaTested ? 1.0f : 0.0f;
        if (wanted != boundAlphaTest)
        {
            vkCmdPushConstants(command, m_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
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
    if (m_descriptorPool)
        vkDestroyDescriptorPool(device.device(), m_descriptorPool, nullptr);
    if (m_descriptorLayout)
        vkDestroyDescriptorSetLayout(device.device(), m_descriptorLayout, nullptr);

    device.destroyBuffer(m_vertexBuffer);
    device.destroyBuffer(m_indexBuffer);
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
