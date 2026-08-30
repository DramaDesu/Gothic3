#include "renderer.h"

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

} // namespace

namespace
{

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

bool CharacterRenderer::create(Device &device, const genome::Actor &actor,
                               const std::vector<SubmeshTextures> &textures, std::string *error)
{
    // Submesh indices are local, so they are rebased while the submeshes are
    // concatenated into one buffer; each keeps its slice for drawing.
    std::vector<std::uint32_t> indices;
    std::uint32_t base = 0;
    for (const genome::ActorSubmesh &submesh : actor.submeshes)
    {
        Part part;
        part.firstIndex = static_cast<std::uint32_t>(indices.size());
        part.indexCount = static_cast<std::uint32_t>(submesh.indices.size());
        m_parts.push_back(part);

        for (std::uint32_t index : submesh.indices)
            indices.push_back(index + base);
        base += static_cast<std::uint32_t>(submesh.vertices.size());
    }
    m_indexCount = indices.size();
    m_vertices.resize(base);

    const VkDeviceSize vertexBytes = sizeof(Vertex) * m_vertices.size();
    for (Buffer &buffer : m_vertexBuffer)
    {
        buffer = device.createBuffer(vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true, error);
        if (!buffer.handle)
            return false;
    }

    m_indexBuffer = device.createBuffer(sizeof(std::uint32_t) * indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true,
                                        error);
    if (!m_indexBuffer.handle)
        return false;
    std::memcpy(m_indexBuffer.mapped, indices.data(), sizeof(std::uint32_t) * indices.size());

    const genome::Image white = solidImage(255, 255, 255, 255);
    const genome::Image flat = solidImage(0, 128, 0, 128); // neutral in the DXT5nm layout
    if (!createTexture(device, white, true, m_white, error) || !createTexture(device, flat, false, m_flat, error))
        return false;

    for (std::size_t index = 0; index < m_parts.size(); ++index)
    {
        const SubmeshTextures wanted = index < textures.size() ? textures[index] : SubmeshTextures{};
        if (wanted.diffuse && !createTexture(device, *wanted.diffuse, true, m_parts[index].diffuse, error))
            return false;
        if (wanted.normal && !createTexture(device, *wanted.normal, false, m_parts[index].normal, error))
            return false;
    }

    VkDescriptorSetLayoutBinding bindings[2]{};
    for (std::uint32_t binding = 0; binding < 2; ++binding)
    {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    vkCreateDescriptorSetLayout(device.device(), &layoutInfo, nullptr, &m_descriptorLayout);

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  static_cast<std::uint32_t>(m_parts.size() * 2)};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = static_cast<std::uint32_t>(m_parts.size());
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    vkCreateDescriptorPool(device.device(), &poolInfo, nullptr, &m_descriptorPool);

    for (Part &part : m_parts)
    {
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = m_descriptorPool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &m_descriptorLayout;
        vkAllocateDescriptorSets(device.device(), &allocate, &part.descriptor);

        const Texture &diffuse = part.diffuse.valid() ? part.diffuse : m_white;
        const Texture &normal = part.normal.valid() ? part.normal : m_flat;
        VkDescriptorImageInfo images[2]{
            {diffuse.sampler, diffuse.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {normal.sampler, normal.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        };
        VkWriteDescriptorSet writes[2]{};
        for (std::uint32_t binding = 0; binding < 2; ++binding)
        {
            writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[binding].dstSet = part.descriptor;
            writes[binding].dstBinding = binding;
            writes[binding].descriptorCount = 1;
            writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[binding].pImageInfo = &images[binding];
        }
        vkUpdateDescriptorSets(device.device(), 2, writes, 0, nullptr);
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
    VkVertexInputAttributeDescription attributes[3]{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, texCoord)},
    };

    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 3;
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

    const VkResult result =
        vkCreateGraphicsPipelines(device.device(), VK_NULL_HANDLE, 1, &info, nullptr, &m_pipeline);
    vkDestroyShaderModule(device.device(), vertexShader, nullptr);
    vkDestroyShaderModule(device.device(), fragmentShader, nullptr);

    if (result != VK_SUCCESS && error)
        *error = "vkCreateGraphicsPipelines failed";
    return result == VK_SUCCESS;
}

void CharacterRenderer::update(Device &device, const genome::Actor &actor, const genome::Skeleton &skeleton,
                               const genome::Motion &motion, float time)
{
    const std::vector<genome::Matrix4> pose = genome::samplePose(skeleton, motion, time);
    const std::vector<genome::Matrix4> skinning = genome::skinningMatrices(actor, skeleton, pose);
    genome::skinVertices(actor, skinning, m_skinned);

    // Normals are skinned with the same matrices; the rotations are rigid, so
    // the rotation part alone is enough.
    std::size_t writeIndex = 0;
    for (const genome::ActorSubmesh &submesh : actor.submeshes)
    {
        for (const genome::ActorVertex &vertex : submesh.vertices)
        {
            Vertex &out = m_vertices[writeIndex];
            out.position = m_skinned[writeIndex];
            out.texCoord = vertex.texCoord;

            std::array<float, 3> normal{};
            float total = 0.0f;
            if (vertex.originalVertex < actor.influences.size())
            {
                for (const genome::SkinInfluence &influence : actor.influences[vertex.originalVertex])
                {
                    if (influence.node >= skinning.size() || influence.weight <= 0.0f)
                        continue;
                    const genome::Matrix4 &m = skinning[influence.node];
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        const float value = m[0 * 4 + axis] * vertex.normal[0] + m[1 * 4 + axis] * vertex.normal[1] +
                                            m[2 * 4 + axis] * vertex.normal[2];
                        normal[axis] += value * influence.weight;
                    }
                    total += influence.weight;
                }
            }
            if (total <= 0.0f)
                normal = vertex.normal;

            const float length =
                std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
            if (length > 0.0f)
                for (float &component : normal)
                    component /= length;
            out.normal = normal;

            ++writeIndex;
        }
    }

    std::memcpy(m_vertexBuffer[device.frameIndex()].mapped, m_vertices.data(), sizeof(Vertex) * m_vertices.size());
}

void CharacterRenderer::draw(Device &device, const std::array<float, 16> &viewProjection,
                             const std::array<float, 4> &light)
{
    VkCommandBuffer command = device.commandBuffer();
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    PushConstants push{viewProjection, light};
    vkCmdPushConstants(command, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);

    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(command, 0, 1, &m_vertexBuffer[device.frameIndex()].handle, &offset);
    vkCmdBindIndexBuffer(command, m_indexBuffer.handle, 0, VK_INDEX_TYPE_UINT32);

    // One draw per submesh, because each carries its own material.
    for (const Part &part : m_parts)
    {
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &part.descriptor, 0,
                                nullptr);
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

    for (Buffer &buffer : m_vertexBuffer)
        device.destroyBuffer(buffer);
    device.destroyBuffer(m_indexBuffer);
    if (m_pipeline)
        vkDestroyPipeline(device.device(), m_pipeline, nullptr);
    if (m_layout)
        vkDestroyPipelineLayout(device.device(), m_layout, nullptr);
}

} // namespace render
