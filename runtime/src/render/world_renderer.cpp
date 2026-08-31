#include "world_renderer.h"

#include <cstring>
#include <fstream>
#include <limits>

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

} // namespace

bool WorldRenderer::create(Device &device, const std::vector<genome::Mesh> &meshes, std::string *error)
{
    std::vector<WorldVertex> vertices;
    std::vector<std::uint32_t> indices;

    m_boundsMin = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max()};
    m_boundsMax = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                   std::numeric_limits<float>::lowest()};

    // Landscape vertices already sit in world space, so the tiles can simply be
    // concatenated - only the indices need rebasing.
    for (const genome::Mesh &mesh : meshes)
    {
        for (const genome::MeshElement &element : mesh.elements)
        {
            if (element.positions.empty() || element.indices.empty())
                continue;

            Range range;
            range.firstIndex = static_cast<std::uint32_t>(indices.size());
            range.indexCount = static_cast<std::uint32_t>(element.indices.size());
            m_ranges.push_back(range);

            const std::uint32_t base = static_cast<std::uint32_t>(vertices.size());
            for (std::size_t index = 0; index < element.positions.size(); ++index)
            {
                WorldVertex vertex{};
                vertex.position = element.positions[index];
                vertex.normal = index < element.normals.size() ? element.normals[index]
                                                               : std::array<float, 3>{0.0f, 1.0f, 0.0f};
                vertex.texCoord = index < element.texCoords.size() ? element.texCoords[index]
                                                                   : std::array<float, 2>{0.0f, 0.0f};
                vertices.push_back(vertex);

                for (int axis = 0; axis < 3; ++axis)
                {
                    m_boundsMin[axis] = std::min(m_boundsMin[axis], vertex.position[axis]);
                    m_boundsMax[axis] = std::max(m_boundsMax[axis], vertex.position[axis]);
                }
            }

            for (std::uint32_t index : element.indices)
                indices.push_back(index + base);
        }
    }

    if (vertices.empty())
    {
        if (error)
            *error = "no geometry to draw";
        return false;
    }

    m_vertexCount = vertices.size();
    m_indexCount = indices.size();

    m_vertexBuffer = device.createBuffer(sizeof(WorldVertex) * vertices.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true,
                                         error);
    if (!m_vertexBuffer.handle)
        return false;
    std::memcpy(m_vertexBuffer.mapped, vertices.data(), sizeof(WorldVertex) * vertices.size());

    m_indexBuffer =
        device.createBuffer(sizeof(std::uint32_t) * indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true, error);
    if (!m_indexBuffer.handle)
        return false;
    std::memcpy(m_indexBuffer.mapped, indices.data(), sizeof(std::uint32_t) * indices.size());

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

    VkVertexInputBindingDescription binding{0, sizeof(WorldVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[3]{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(WorldVertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(WorldVertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(WorldVertex, texCoord)},
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

void WorldRenderer::draw(Device &device, const std::array<float, 16> &viewProjection,
                         const std::array<float, 4> &light)
{
    VkCommandBuffer command = device.commandBuffer();
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    PushConstants push{viewProjection, light};
    vkCmdPushConstants(command, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                       &push);

    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(command, 0, 1, &m_vertexBuffer.handle, &offset);
    vkCmdBindIndexBuffer(command, m_indexBuffer.handle, 0, VK_INDEX_TYPE_UINT32);

    // One draw for the lot while every tile shares a pipeline; the ranges are
    // kept so per-material draws can be split out once materials arrive.
    vkCmdDrawIndexed(command, static_cast<std::uint32_t>(m_indexCount), 1, 0, 0, 0);
}

void WorldRenderer::destroy(Device &device)
{
    device.destroyBuffer(m_vertexBuffer);
    device.destroyBuffer(m_indexBuffer);
    if (m_pipeline)
        vkDestroyPipeline(device.device(), m_pipeline, nullptr);
    if (m_layout)
        vkDestroyPipelineLayout(device.device(), m_layout, nullptr);
    m_pipeline = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
}

} // namespace render
