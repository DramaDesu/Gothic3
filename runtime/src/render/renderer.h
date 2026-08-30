#pragma once

// Draws an assembled character. Vertices are uploaded once in bind pose and
// carry their bone bindings; each frame only the bone matrices change, so the
// skinning itself happens in the vertex shader.

#include "genome/motion.h"
#include "texture.h"
#include "vulkan.h"

#include <array>
#include <string>
#include <vector>

namespace render
{

// Four bones per vertex is the usual GPU budget; the data goes up to twenty, so
// the heaviest four are kept and renormalised.
constexpr std::uint32_t c_MaxInfluences = 4;

struct Vertex
{
    std::array<float, 3> position;
    std::array<float, 3> normal;
    std::array<float, 2> texCoord;
    std::array<std::uint16_t, c_MaxInfluences> bones;
    std::array<float, c_MaxInfluences> weights;
};

// One submesh: its slice of the index buffer, the textures its material
// resolved to, and where its piece's bone matrices start.
struct Part
{
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t boneBase = 0;
    Texture diffuse;
    Texture normal;
    VkDescriptorSet descriptor[Device::c_FramesInFlight]{};
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

    // Poses every piece from the same skeleton and uploads this frame's bone
    // matrices. The vertex data itself never changes.
    void update(Device &device, const std::vector<Piece> &pieces, const genome::Skeleton &skeleton,
                const genome::Motion &motion, float time);

    void draw(Device &device, const std::array<float, 16> &viewProjection, const std::array<float, 4> &light);

    std::size_t vertexCount() const { return m_vertexCount; }
    std::size_t indexCount() const { return m_indexCount; }
    std::size_t boneCount() const { return m_boneCount; }

  private:
    bool createPipeline(Device &device, std::string *error);

    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;

    Buffer m_vertexBuffer{}; // uploaded once, in bind pose
    Buffer m_indexBuffer{};
    Buffer m_boneBuffer[Device::c_FramesInFlight]{}; // rewritten each frame
    std::size_t m_vertexCount = 0;
    std::size_t m_indexCount = 0;
    std::size_t m_boneCount = 0;

    VkDescriptorSetLayout m_descriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::vector<Part> m_parts;
    Texture m_white; // stand-in when a material has no texture
    Texture m_flat;  // stand-in normal map

    std::vector<genome::Matrix4> m_matrices; // scratch, refilled each frame
};

} // namespace render
