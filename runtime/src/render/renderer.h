#pragma once

// Draws one skinned character. The pose is applied on the CPU and uploaded each
// frame, which is fast enough for a viewer and keeps the GPU side minimal until
// there is a reason to move skinning onto it.

#include "genome/motion.h"
#include "texture.h"
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

// One submesh: its slice of the index buffer plus the textures its material
// resolved to.
struct Part
{
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    Texture diffuse;
    Texture normal;
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
};

class CharacterRenderer
{
  public:
    // Either image may be absent, in which case a neutral stand-in is used.
    struct SubmeshTextures
    {
        const genome::Image *diffuse = nullptr;
        const genome::Image *normal = nullptr;
    };

    // A character is assembled from several actors sharing one skeleton: a body,
    // a head, and whatever the slots carry.
    struct Piece
    {
        const genome::Actor *actor = nullptr;
        std::vector<SubmeshTextures> textures; // one per submesh
    };

    bool create(Device &device, const std::vector<Piece> &pieces, std::string *error);
    void destroy(Device &device);

    // Poses every piece from the same skeleton and refreshes this frame's buffer.
    void update(Device &device, const std::vector<Piece> &pieces, const genome::Skeleton &skeleton,
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

    VkDescriptorSetLayout m_descriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::vector<Part> m_parts;
    Texture m_white;  // stand-in when a material has no texture
    Texture m_flat;   // stand-in normal map

    std::vector<Vertex> m_vertices;             // rebuilt per frame
    std::vector<std::array<float, 3>> m_skinned; // scratch for CPU skinning
};

} // namespace render
