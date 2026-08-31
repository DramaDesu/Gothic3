#pragma once

// Draws static world geometry. Nothing here is posed, so vertices are uploaded
// once in their own object space and repeated through instance transforms - a
// world of a hundred thousand objects only has a few thousand distinct meshes.

#include "genome/image.h"
#include "genome/mesh.h"
#include "genome/world.h"
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

// One mesh and every place it appears. A landscape tile is just an instance
// count of one with an identity transform.
struct MeshInstances
{
    const genome::Mesh *mesh = nullptr;
    std::vector<genome::WorldMatrix> transforms;
    std::vector<const genome::Image *> textures; // one per mesh element, may be null
};

class WorldRenderer
{
  public:
    bool create(Device &device, const std::vector<MeshInstances> &batches, std::string *error);
    void destroy(Device &device);

    void draw(Device &device, const std::array<float, 16> &viewProjection, const std::array<float, 4> &light);

    std::size_t vertexCount() const { return m_vertexCount; }
    std::size_t triangleCount() const { return m_indexCount / 3; }
    std::size_t instanceCount() const { return m_instanceCount; }
    std::size_t drawCount() const { return m_ranges.size(); }

    const std::array<float, 3> &boundsMin() const { return m_boundsMin; }
    const std::array<float, 3> &boundsMax() const { return m_boundsMax; }

  private:
    bool createPipeline(Device &device, std::string *error);

    // One draw: an element of one mesh, repeated over that mesh's instances.
    struct Range
    {
        std::uint32_t firstIndex = 0;
        std::uint32_t indexCount = 0;
        std::int32_t vertexOffset = 0;
        std::uint32_t firstInstance = 0;
        std::uint32_t instanceCount = 0;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
    };

    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;

    Buffer m_vertexBuffer{};
    Buffer m_indexBuffer{};
    Buffer m_instanceBuffer{};
    std::size_t m_vertexCount = 0;
    std::size_t m_indexCount = 0;
    std::size_t m_instanceCount = 0;
    std::vector<Range> m_ranges;

    VkDescriptorSetLayout m_descriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::vector<Texture> m_textures;
    Texture m_white;

    std::array<float, 3> m_boundsMin{};
    std::array<float, 3> m_boundsMax{};
};

} // namespace render
