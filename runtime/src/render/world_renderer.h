#pragma once

// Draws static world geometry. Nothing here is posed, so vertices are uploaded
// once in their own object space and repeated through instance transforms - a
// world of a hundred thousand objects only has a few thousand distinct meshes.

#include "genome/image.h"
#include "genome/mesh.h"
#include "genome/world.h"
#include "occlusion.h"
#include "profile.h"
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

    // The distance band this batch is drawn in, as a way of switching detail: a
    // definition is loaded twice, a full mesh for near and a thinned one for
    // far, and each instance is drawn by whichever band it falls in. Zero far
    // means no limit.
    float lodNear = 0.0f;
    float lodFar = 0.0f;

    // Billboards: the mesh is a unit quad and its transform is rebuilt each
    // frame to face the camera, sized from the instance's own bounds. The
    // instance buffer is refilled every frame anyway, so this costs nothing that
    // was not already being paid.
    bool faceCamera = false;

    // Where this instance's baked lighting starts in the shared buffer, or -1
    // for none. Lighting is per instance and vertices are shared between
    // instances, so it cannot live in the vertex buffer: the shader reads it by
    // index instead. One per transform.
    std::vector<std::int32_t> lightmapBase;

    // Whether these instances may hide what is behind them. A bounding box is a
    // fair stand-in for a house and a poor one for a tree, whose box is mostly
    // air - so foliage is drawn but never rasterised as an occluder.
    bool occludes = true;
};

class WorldRenderer
{
  public:
    // Timestamps around the drawing, collected by the profiler. Call collect
    // once a frame, outside a render pass.
    void startProfiling(Device &device);
    void collectProfiling(Device &device);
    void stopProfiling();

    bool create(Device &device, const std::vector<MeshInstances> &batches, std::string *error);
    void destroy(Device &device);

    // Rebuilds this frame's instance buffer from what the camera can see.
    // `eye` and `pixelsPerRadian` let small distant objects be dropped: a fork
    // inside a house a kilometre away passes a frustum test but covers no
    // pixels, and there are tens of thousands of those.
    void cull(Device &device, const std::array<float, 16> &viewProjection, const std::array<float, 3> &eye,
              float pixelsPerRadian, float minimumPixels, bool useOcclusion);

    void draw(Device &device, const std::array<float, 16> &viewProjection, const std::array<float, 4> &light);

    // The world's static point lights. There are 588 of them across the whole
    // map, so the nearest handful to the camera are picked each frame and
    // handed to the shader; a light beyond its own range lights nothing.
    void setLights(const std::vector<genome::PointLight> &lights) { m_lights = lights; }

    // Every instance's baked vertex lighting, concatenated. Call before create.
    void setLightmaps(std::vector<std::uint32_t> colours) { m_lightmapColours = std::move(colours); }

    // The direction the baked light arrived from, three floats a vertex, in the
    // same order as the colours. Without it the bake can only brighten; with it
    // a surface answers to where the light was.
    void setLightmapDirections(std::vector<float> incident) { m_lightmapIncident = std::move(incident); }
    std::size_t litInstances() const { return m_litLights; }

    // Baking a billboard needs one batch at a time into its own viewport, with
    // every instance present rather than the visible subset - so these two step
    // around cull() instead of through it.
    void prepareAll(Device &device);
    void drawBatch(Device &device, std::size_t batch, const std::array<float, 16> &viewProjection,
                   VkCommandBuffer command);

    std::size_t batchCount() const { return m_batches.size(); }
    const std::array<float, 6> *batchExtent(std::size_t batch) const;

    std::size_t visibleInstances() const { return m_visibleInstances; }
    std::size_t tooSmallInstances() const { return m_tooSmall; }
    std::size_t occludedInstances() const { return m_occluded; }

    std::size_t vertexCount() const { return m_vertexCount; }
    std::size_t triangleCount() const { return m_indexCount / 3; }
    std::size_t instanceCount() const { return m_instanceCount; }
    std::size_t drawCount() const { return m_ranges.size(); }

    // What the last cull actually handed the card: triangles times instances,
    // which is the number that decides a GPU-bound frame rather than the
    // instance count.
    std::size_t submittedTriangles() const { return m_submittedTriangles; }
    std::size_t submittedDraws() const { return m_submittedDraws; }

    // How many instances the last cull actually looked at, as opposed to
    // rejected a whole batch at a time. This is what the cull costs.
    std::size_t testedInstances() const { return m_testedInstances; }

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

    // Up to this many lights reach the shader in one frame. The world has 588
    // and their ranges are 3 to 40 metres, so nowhere sees many at once.
    static constexpr std::uint32_t c_MaxFrameLights = 16;

    std::vector<genome::PointLight> m_lights;
    std::vector<std::uint32_t> m_lightmapColours;
    std::vector<float> m_lightmapIncident;
    Buffer m_lightmapBuffer{};
    Buffer m_incidentBuffer{};
    std::size_t m_litLights = 0;
    Buffer m_lightBuffer[Device::c_FramesInFlight]{};
    VkDescriptorSetLayout m_lightLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_lightSet[Device::c_FramesInFlight]{};
    VkDescriptorPool m_lightPool = VK_NULL_HANDLE;

    GpuContext m_gpu = nullptr;
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
        float lodNear = 0.0f;
        float lodFar = 0.0f;
        bool faceCamera = false;
    };
    std::vector<Batch> m_batches;

    // A grid over the world, one entry per occupied cell, holding the batches
    // that sit inside it. From an overview of the whole map every batch is
    // inside the frustum, so testing batches one at a time rejects nothing and
    // the cull ends up walking every instance; testing a cell rejects the
    // hundreds of batches in it at once. The largest instance in a cell decides
    // whether the cell is too small to bother with, so a cell of grass goes in
    // one test and a cell with a house in it does not.
    struct Cell
    {
        std::array<float, 6> bounds{};
        float largestRadius = 0.0f;
        std::vector<std::size_t> batches;
    };
    std::vector<Cell> m_cells;
    std::vector<std::size_t> m_looseBatches; // no bounds, or spanning the map
    void buildGrid();
    std::vector<genome::WorldMatrix> m_visible; // scratch, refilled each frame
    std::size_t m_visibleInstances = 0;
    std::size_t m_tooSmall = 0;
    std::size_t m_occluded = 0;
    std::size_t m_submittedTriangles = 0;
    std::size_t m_submittedDraws = 0;
    std::size_t m_testedInstances = 0;
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
