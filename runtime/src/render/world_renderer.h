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
    // World bounds per transform, used to decide visibility. Empty means the
    // batch is always drawn.
    std::vector<std::array<float, 6>> bounds;
    std::vector<const genome::Image *> textures; // one per mesh element, may be null
};

class WorldRenderer
{
  public:
    bool create(Device &device, const std::vector<MeshInstances> &batches, std::string *error);
    void destroy(Device &device);

    // Rebuilds this frame's instance buffer from what the camera can see.
    void cull(Device &device, const std::array<float, 16> &viewProjection);

    void draw(Device &device, const std::array<float, 16> &viewProjection, const std::array<float, 4> &light);

    std::size_t visibleInstances() const { return m_visibleInstances; }

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
    Buffer m_instanceBuffer[Device::c_FramesInFlight]{};
    std::size_t m_vertexCount = 0;
    std::size_t m_indexCount = 0;
    std::size_t m_instanceCount = 0;
    std::vector<Range> m_ranges;

    // Per batch: the transforms and their bounds, kept so each frame can pick
    // the visible subset.
    struct Batch
    {
        std::vector<genome::WorldMatrix> transforms;
        std::vector<std::array<float, 6>> bounds;
        std::vector<std::size_t> ranges; // indices into m_ranges
    };
    std::vector<Batch> m_batches;
    std::vector<genome::WorldMatrix> m_visible; // scratch, refilled each frame
    std::size_t m_visibleInstances = 0;

    VkDescriptorSetLayout m_descriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::vector<Texture> m_textures;
    Texture m_white;

    std::array<float, 3> m_boundsMin{};
    std::array<float, 3> m_boundsMax{};
};

} // namespace render
