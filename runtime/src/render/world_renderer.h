#pragma once

// Draws static world geometry. Unlike a character, nothing here is posed: the
// vertices are already in world space, so they are uploaded once and only the
// camera moves.

#include "genome/mesh.h"
#include "texture.h"
#include "vulkan.h"

#include <array>
#include <string>
#include <vector>

namespace render
{

struct WorldVertex
{
    std::array<float, 3> position;
    std::array<float, 3> normal;
    std::array<float, 2> texCoord;
};

class WorldRenderer
{
  public:
    // Each mesh keeps its own draw range so materials can be attached later;
    // for now they all share one pipeline.
    bool create(Device &device, const std::vector<genome::Mesh> &meshes, std::string *error);
    void destroy(Device &device);

    void draw(Device &device, const std::array<float, 16> &viewProjection, const std::array<float, 4> &light);

    std::size_t vertexCount() const { return m_vertexCount; }
    std::size_t triangleCount() const { return m_indexCount / 3; }
    std::size_t drawCount() const { return m_ranges.size(); }

    const std::array<float, 3> &boundsMin() const { return m_boundsMin; }
    const std::array<float, 3> &boundsMax() const { return m_boundsMax; }

  private:
    bool createPipeline(Device &device, std::string *error);

    struct Range
    {
        std::uint32_t firstIndex = 0;
        std::uint32_t indexCount = 0;
    };

    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;

    Buffer m_vertexBuffer{};
    Buffer m_indexBuffer{};
    std::size_t m_vertexCount = 0;
    std::size_t m_indexCount = 0;
    std::vector<Range> m_ranges;

    std::array<float, 3> m_boundsMin{};
    std::array<float, 3> m_boundsMax{};
};

} // namespace render
