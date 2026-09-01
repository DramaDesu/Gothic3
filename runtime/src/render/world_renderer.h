#pragma once

// Draws static world geometry. Nothing here is posed, so vertices are uploaded
// once in their own object space and repeated through instance transforms - a
// world of a hundred thousand objects only has a few thousand distinct meshes.

#include "genome/image.h"
#include "genome/mesh.h"
#include "genome/world.h"
#include "occlusion.h"
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

    // Which elements throw away their transparent pixels. Foliage must - it is
    // drawn as quads whose texture is mostly empty - and solid surfaces must not:
    // a stone wall whose texture carries anything but 1 in its alpha channel
    // comes out riddled with holes. One per mesh element; empty means none.
    std::vector<char> alphaTested;

    // Whether these instances may hide what is behind them. A bounding box is a
    // fair stand-in for a house and a poor one for a tree, whose box is mostly
    // air - so foliage is drawn but never rasterised as an occluder.
    bool occludes = true;
};

class WorldRenderer
{
  public:
    bool create(Device &device, const std::vector<MeshInstances> &batches, std::string *error);
    void destroy(Device &device);

    // Rebuilds this frame's instance buffer from what the camera can see.
    // `eye` and `pixelsPerRadian` let small distant objects be dropped: a fork
    // inside a house a kilometre away passes a frustum test but covers no
    // pixels, and there are tens of thousands of those.
    void cull(Device &device, const std::array<float, 16> &viewProjection, const std::array<float, 3> &eye,
              float pixelsPerRadian, float minimumPixels, bool useOcclusion);

    void draw(Device &device, const std::array<float, 16> &viewProjection, const std::array<float, 4> &light);

    std::size_t visibleInstances() const { return m_visibleInstances; }
    std::size_t tooSmallInstances() const { return m_tooSmall; }
    std::size_t occludedInstances() const { return m_occluded; }

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
        bool alphaTested = false;
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
        bool occludes = true;

        // Everything this batch's instances cover. A grass patch or a tree
        // clump is local, so one test against this rejects all of its instances
        // at once; a mesh scattered across the map spans everything and falls
        // through to the per-instance tests, which is the honest outcome.
        std::array<float, 6> extent{};
        bool hasExtent = false;
    };
    std::vector<Batch> m_batches;
    std::vector<genome::WorldMatrix> m_visible; // scratch, refilled each frame
    std::size_t m_visibleInstances = 0;
    std::size_t m_tooSmall = 0;
    std::size_t m_occluded = 0;
    OcclusionBuffer m_occlusion;

    // Instances big enough to hide other things: rasterised first, then used to
    // reject the rest.
    struct Occluder
    {
        std::size_t batch = 0;
        std::size_t instance = 0;
    };
    std::vector<Occluder> m_occluders;

    VkDescriptorSetLayout m_descriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::vector<Texture> m_textures;
    Texture m_white;

    std::array<float, 3> m_boundsMin{};
    std::array<float, 3> m_boundsMax{};
};

} // namespace render
