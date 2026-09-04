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

    // One drawn thing: an actor, its textures, and what poses and places it.
    //
    // A character is often several of these - a body and a head - and they
    // belong together by carrying the same skeleton and the same world matrix
    // rather than by being grouped. That is what lets one renderer hold a crowd:
    // the pieces of one person and the people themselves are the same list.
    struct Piece
    {
        const genome::Actor *actor = nullptr;
        std::vector<SubmeshTextures> textures; // one per submesh

        // Where the piece is posed from. The skeleton belongs to the body actor
        // even when the piece is a head, since a head's own skeleton is shorter
        // and posing with it misplaces bones by tens of centimetres.
        const genome::Skeleton *skeleton = nullptr;
        const genome::Motion *motion = nullptr;
        float time = 0.0f;

        // Folded into the skinning matrices rather than the view, because the
        // vertex shader carries the normal through the skinning matrix
        // separately from the position: a rotation applied only to the view
        // would turn the body and leave its lighting pointing the old way.
        genome::Matrix4 world{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    };

    bool create(Device &device, const std::vector<Piece> &pieces, std::string *error);
    void destroy(Device &device);

    // Poses every piece from its own skeleton and uploads this frame's bone
    // matrices. The vertex data itself never changes.
    //
    // Consecutive pieces sharing a skeleton, clip and time are posed once
    // between them: a body and its head cost one sampling, not two.
    void update(Device &device, const std::vector<Piece> &pieces);

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
