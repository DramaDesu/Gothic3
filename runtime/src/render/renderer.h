#pragma once

// Draws one skinned character. The pose is applied on the CPU and uploaded each
// frame, which is fast enough for a viewer and keeps the GPU side minimal until
// there is a reason to move skinning onto it.

#include "genome/motion.h"
#include "vulkan.h"

#include <array>
#include <string>
#include <vector>

namespace render
{

struct Vertex
{
    std::array<float, 3> position;
    std::array<float, 3> normal;
    std::array<float, 2> texCoord;
};

class CharacterRenderer
{
  public:
    bool create(Device &device, const genome::Actor &actor, std::string *error);
    void destroy(Device &device);

    // Poses the character and refreshes the vertex buffer for this frame.
    void update(Device &device, const genome::Actor &actor, const genome::Skeleton &skeleton,
                const genome::Motion &motion, float time);

    void draw(Device &device, const std::array<float, 16> &viewProjection, const std::array<float, 4> &light);

    std::size_t vertexCount() const { return m_vertices.size(); }
    std::size_t indexCount() const { return m_indexCount; }

  private:
    bool createPipeline(Device &device, std::string *error);

    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;

    Buffer m_vertexBuffer[Device::c_FramesInFlight]{};
    Buffer m_indexBuffer{};
    std::size_t m_indexCount = 0;

    std::vector<Vertex> m_vertices;             // rebuilt per frame
    std::vector<std::array<float, 3>> m_skinned; // scratch for CPU skinning
};

} // namespace render
