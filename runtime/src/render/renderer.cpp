#include "renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace render
{
namespace
{

struct PushConstants
{
    std::array<float, 16> viewProjection;
    std::array<float, 4> light;
    std::uint32_t boneBase;
};

std::vector<char> readFile(const std::string &path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return {};
    const std::streamsize size = file.tellg();
    file.seekg(0);
    std::vector<char> bytes(static_cast<std::size_t>(size));
    file.read(bytes.data(), size);
    return bytes;
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

// A one-pixel image standing in for a missing map, so the shader never has to
// ask whether a texture exists.
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

bool CharacterRenderer::create(Device &device, const std::vector<Piece> &pieces, std::string *error)
{
    // Vertices from every piece land in one buffer, already carrying their bone
    // indices, so the GPU can pose them without the CPU touching them again.
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::uint32_t vertexBase = 0;
    std::uint32_t boneBase = 0;

    for (const Piece &piece : pieces)
    {
        if (!piece.actor)
            continue;

        for (const genome::ActorSubmesh &submesh : piece.actor->submeshes)
        {
            Part part;
            part.firstIndex = static_cast<std::uint32_t>(indices.size());
            part.indexCount = static_cast<std::uint32_t>(submesh.indices.size());
            part.boneBase = boneBase;
            m_parts.push_back(part);

            for (const genome::ActorVertex &source : submesh.vertices)
            {
                Vertex vertex{};
                vertex.position = source.position;
                vertex.normal = source.normal;
                vertex.texCoord = source.texCoord;

                // Influences are sorted heaviest first at load time; keep four
                // and renormalise so the shader can blend blindly.
                float total = 0.0f;
                if (source.originalVertex < piece.actor->influences.size())
                {
                    const auto &list = piece.actor->influences[source.originalVertex];
                    for (std::size_t slot = 0; slot < c_MaxInfluences && slot < list.size(); ++slot)
                    {
                        vertex.bones[slot] = list[slot].node;
                        vertex.weights[slot] = std::max(list[slot].weight, 0.0f);
                        total += vertex.weights[slot];
                    }
                }
                if (total > 0.0f)
                    for (float &weight : vertex.weights)
                        weight /= total;
                else
                    vertex.weights[0] = 1.0f;

                vertices.push_back(vertex);
            }

            for (std::uint32_t index : submesh.indices)
                indices.push_back(index + vertexBase);
            vertexBase += static_cast<std::uint32_t>(submesh.vertices.size());
        }

        boneBase += static_cast<std::uint32_t>(piece.actor->nodes.size());
    }

    m_vertexCount = vertices.size();
    m_indexCount = indices.size();
    m_boneCount = boneBase;
    m_matrices.resize(m_boneCount);

    m_vertexBuffer = device.createBuffer(sizeof(Vertex) * vertices.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true,
                                         error);
    if (!m_vertexBuffer.handle)
        return false;
    std::memcpy(m_vertexBuffer.mapped, vertices.data(), sizeof(Vertex) * vertices.size());

    m_indexBuffer = device.createBuffer(sizeof(std::uint32_t) * indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true,
                                        error);
    if (!m_indexBuffer.handle)
        return false;
    std::memcpy(m_indexBuffer.mapped, indices.data(), sizeof(std::uint32_t) * indices.size());

    for (Buffer &buffer : m_boneBuffer)
    {
        buffer = device.createBuffer(sizeof(genome::Matrix4) * std::max<std::size_t>(m_boneCount, 1),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, error);
        if (!buffer.handle)
            return false;
    }

    const genome::Image white = solidImage(255, 255, 255, 255);
    const genome::Image flat = solidImage(0, 128, 0, 128); // neutral in the DXT5nm layout
    if (!createTexture(device, white, true, m_white, error) || !createTexture(device, flat, false, m_flat, error))
        return false;

    std::size_t partIndex = 0;
    for (const Piece &piece : pieces)
    {
        if (!piece.actor)
            continue;
        for (std::size_t submesh = 0; submesh < piece.actor->submeshes.size(); ++submesh, ++partIndex)
        {
            const SubmeshTextures wanted =
                submesh < piece.textures.size() ? piece.textures[submesh] : SubmeshTextures{};
            if (wanted.diffuse && !createTexture(device, *wanted.diffuse, true, m_parts[partIndex].diffuse, error))
                return false;
            if (wanted.normal && !createTexture(device, *wanted.normal, false, m_parts[partIndex].normal, error))
                return false;
        }
    }

    VkDescriptorSetLayoutBinding bindings[3]{};
    for (std::uint32_t binding = 0; binding < 2; ++binding)
    {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;
    vkCreateDescriptorSetLayout(device.device(), &layoutInfo, nullptr, &m_descriptorLayout);

    // A set per part per frame: the textures never change, but the bone buffer
    // alternates with the frame in flight.
    const std::uint32_t setCount = static_cast<std::uint32_t>(m_parts.size()) * Device::c_FramesInFlight;
    VkDescriptorPoolSize poolSizes[2]{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, setCount * 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, setCount},
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = setCount;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    vkCreateDescriptorPool(device.device(), &poolInfo, nullptr, &m_descriptorPool);

    for (Part &part : m_parts)
    {
        for (std::uint32_t frame = 0; frame < Device::c_FramesInFlight; ++frame)
        {
            VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            allocate.descriptorPool = m_descriptorPool;
            allocate.descriptorSetCount = 1;
            allocate.pSetLayouts = &m_descriptorLayout;
            vkAllocateDescriptorSets(device.device(), &allocate, &part.descriptor[frame]);

            const Texture &diffuse = part.diffuse.valid() ? part.diffuse : m_white;
            const Texture &normal = part.normal.valid() ? part.normal : m_flat;
            VkDescriptorImageInfo images[2]{
                {diffuse.sampler, diffuse.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {normal.sampler, normal.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            };
            VkDescriptorBufferInfo bones{m_boneBuffer[frame].handle, 0, VK_WHOLE_SIZE};

            VkWriteDescriptorSet writes[3]{};
            for (std::uint32_t binding = 0; binding < 2; ++binding)
            {
                writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[binding].dstSet = part.descriptor[frame];
                writes[binding].dstBinding = binding;
                writes[binding].descriptorCount = 1;
                writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[binding].pImageInfo = &images[binding];
            }
            writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[2].dstSet = part.descriptor[frame];
            writes[2].dstBinding = 2;
            writes[2].descriptorCount = 1;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[2].pBufferInfo = &bones;

            vkUpdateDescriptorSets(device.device(), 3, writes, 0, nullptr);
        }
    }

    return createPipeline(device, error);
}

bool CharacterRenderer::createPipeline(Device &device, std::string *error)
{
    VkShaderModule vertexShader = loadShader(device.device(), "shaders/mesh.vert.spv");
    VkShaderModule fragmentShader = loadShader(device.device(), "shaders/mesh.frag.spv");
    if (!vertexShader || !fragmentShader)
    {
        if (error)
            *error = "cannot load shaders/mesh.{vert,frag}.spv";
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

    VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[5]{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, texCoord)},
        {3, 0, VK_FORMAT_R16G16B16A16_UINT, offsetof(Vertex, bones)},
        {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, weights)},
    };

    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 5;
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    // Both faces for now: the data's winding is documented but a viewer that
    // silently drops half a model is worse than one that shows too much.
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

    if (result != VK_SUCCESS && error)
        *error = "vkCreateGraphicsPipelines failed";
    return result == VK_SUCCESS;
}

namespace
{
// Column major, the way genome::Matrix4 is: c[col][row] = sum over k of
// a[k][row] * b[col][k]. genome has one of these in motion.cpp but it is in an
// anonymous namespace there and not exported.
genome::Matrix4 concat(const genome::Matrix4 &a, const genome::Matrix4 &b)
{
    genome::Matrix4 out{};
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

void CharacterRenderer::update(Device &device, const std::vector<Piece> &pieces)
{
    // Each piece resolves a pose against its own node order and bind pose, which
    // is what lets a head follow the body's skeleton. The result is one flat
    // array the shader indexes with a per-part base.
    //
    // The pose is sampled once for a run of pieces that share a skeleton, clip
    // and time - a body and its head - since sampling is the expensive half and
    // they would sample the same thing.
    std::vector<genome::Matrix4> pose;
    const genome::Skeleton *posedFrom = nullptr;
    const genome::Motion *posedBy = nullptr;
    float posedAt = -1.0f;

    std::size_t base = 0;
    for (const Piece &piece : pieces)
    {
        if (!piece.actor || !piece.skeleton || !piece.motion)
            continue;
        if (piece.skeleton != posedFrom || piece.motion != posedBy || piece.time != posedAt)
        {
            pose = genome::samplePose(*piece.skeleton, *piece.motion, piece.time);
            posedFrom = piece.skeleton;
            posedBy = piece.motion;
            posedAt = piece.time;
        }
        const std::vector<genome::Matrix4> skinning =
            genome::skinningMatrices(*piece.actor, *piece.skeleton, pose);
        for (std::size_t bone = 0; bone < skinning.size(); ++bone)
            m_matrices[base + bone] = concat(piece.world, skinning[bone]);
        base += skinning.size();
    }

    std::memcpy(m_boneBuffer[device.frameIndex()].mapped, m_matrices.data(),
                sizeof(genome::Matrix4) * m_matrices.size());
}

void CharacterRenderer::draw(Device &device, const std::array<float, 16> &viewProjection,
                             const std::array<float, 4> &light)
{
    VkCommandBuffer command = device.commandBuffer();
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(command, 0, 1, &m_vertexBuffer.handle, &offset);
    vkCmdBindIndexBuffer(command, m_indexBuffer.handle, 0, VK_INDEX_TYPE_UINT32);

    // One draw per submesh, since each carries its own material.
    for (const Part &part : m_parts)
    {
        PushConstants push{viewProjection, light, part.boneBase};
        vkCmdPushConstants(command, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1,
                                &part.descriptor[device.frameIndex()], 0, nullptr);
        vkCmdDrawIndexed(command, part.indexCount, 1, part.firstIndex, 0, 0);
    }
}

void CharacterRenderer::destroy(Device &device)
{
    for (Part &part : m_parts)
    {
        destroyTexture(device, part.diffuse);
        destroyTexture(device, part.normal);
    }
    destroyTexture(device, m_white);
    destroyTexture(device, m_flat);
    if (m_descriptorPool)
        vkDestroyDescriptorPool(device.device(), m_descriptorPool, nullptr);
    if (m_descriptorLayout)
        vkDestroyDescriptorSetLayout(device.device(), m_descriptorLayout, nullptr);

    device.destroyBuffer(m_vertexBuffer);
    device.destroyBuffer(m_indexBuffer);
    for (Buffer &buffer : m_boneBuffer)
        device.destroyBuffer(buffer);

    if (m_pipeline)
        vkDestroyPipeline(device.device(), m_pipeline, nullptr);
    if (m_layout)
        vkDestroyPipelineLayout(device.device(), m_layout, nullptr);
}

} // namespace render
