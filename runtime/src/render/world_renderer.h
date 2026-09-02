#pragma once

// Draws static world geometry. Nothing here is posed, so vertices are uploaded
// once in their own object space and repeated through instance transforms - a
// world of a hundred thousand objects only has a few thousand distinct meshes.

#include "arena.h"
#include "genome/image.h"
#include "genome/mesh.h"
#include "genome/world.h"
#include "occlusion.h"
#include "profile.h"
#include "texture.h"
#include "vulkan.h"

#include <array>
#include <map>
#include <set>
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
    // What the arenas are built to hold. Fixed on purpose: a buffer that cannot
    // grow says at load time that the budget is wrong, instead of saying it at
    // run time by hitching. Every count is in elements.
    struct Budget
    {
        std::size_t vertices = 8u << 20;
        std::size_t indices = 16u << 20;
        // Vertices carrying baked light. The three lighting arenas are one
        // colour, three floats of incident direction and two of patch
        // coordinate per vertex, so this one number sizes all three.
        std::size_t lightVertices = 4u << 20;
        std::size_t instances = 512u << 10;
        std::uint32_t textures = 8192;
        VkDeviceSize staging = 64u << 20;
    };
    // Timestamps around the drawing, collected by the profiler. Call collect
    // once a frame, outside a render pass.
    void startProfiling(Device &device);
    void collectProfiling(Device &device);
    void stopProfiling();

    // An empty world of a fixed size, ready to be filled.
    bool create(Device &device, const Budget &budget, std::string *error);

    // Everything at once, for the tools that load a fixed set and never add to
    // it: the budget is measured from what is handed over.
    bool create(Device &device, const std::vector<MeshInstances> &batches, std::string *error);

    // The baked light one sector brought. All three are indexed by the base
    // an instance carries, so they belong together.
    struct SectorLighting
    {
        std::vector<std::uint32_t> colours;
        std::vector<float> incident;
        std::vector<float> coords;
    };

    // Puts a sector's geometry in. A mesh already present is not uploaded twice
    // - it is shared and counted - so a sector arriving next to one already
    // loaded pays only for what is new to it. Instances carry the sector, which
    // is what lets them be taken out again without giving every sector its own
    // batches and its own draws.
    bool addSector(Device &device, std::uint32_t sector, const std::vector<MeshInstances> &batches,
                   const SectorLighting &lighting, std::string *error);

    // Takes one back. The meshes only nothing else is using go with it.
    void dropSector(Device &device, std::uint32_t sector);

    bool sectorResident(std::uint32_t sector) const;
    std::size_t sectorCount() const { return m_sectors.size(); }

    // Writes rectangles of the baked-patch atlas, for the patches a sector
    // brings with it. The texture and its descriptor do not change.
    bool updatePatchAtlas(Device &device, const std::vector<TextureRegion> &regions, std::string *error);

    void destroy(Device &device);

    // How much of each arena is spoken for, for whoever set the budget.
    void reportArenas() const;

    // What the textures hold, and how much of it belongs to no batch that is
    // still here - which is what evicting them would give back.
    VkDeviceSize textureBytes() const;
    std::size_t textureCount() const { return m_textureOf.size(); }
    std::size_t texturesFreed() const { return m_texturesFreed; }
    // Of those, how many were later wanted again and uploaded a second time -
    // the one path that only exists once textures can be freed.
    std::size_t texturesRemade() const { return m_texturesRemade; }
    // The most handles destroyed on any one frame, and the most arena ranges.
    // A burst of departures makes a burst of these three frames later.
    std::size_t worstRetireBurst() const { return m_worstRetireBurst; }

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

    // Where each vertex of each instance samples the baked light: two floats
    // into the atlas below, in step with the colours. A negative u means the
    // vertex has no baked patch.
    void setLightmapCoords(std::vector<float> coords) { m_lightmapCoords = std::move(coords); }
    void setLightmapAtlas(const genome::Image *atlas) { m_lightmapAtlas = atlas; }
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

    // Frees what has outlived the frames in flight. Called every frame.
    void retireReleases(Device &device, std::uint64_t frame);
    std::size_t pendingReleases() const { return m_pendingReleases.size(); }

    // How many arrivals landed while a departure had already shifted the batch
    // indices, and how many lookups that would actually have got wrong - the
    // second is the one that says the bug was real rather than possible.
    std::size_t staleArrivals() const { return m_staleArrivals; }
    std::size_t staleLookups() const { return m_staleLookups; }
    std::size_t staleOutOfRange() const { return m_staleOutOfRange; }

    std::size_t vertexCount() const { return m_vertexCount; }
    std::size_t triangleCount() const { return m_indexCount / 3; }
    std::size_t instanceCount() const { return m_instanceCount; }
    std::size_t drawCount() const { return m_rangeCount; }

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
        // Which image that set was made from, so a batch that goes can give
        // its textures back.
        const genome::Image *image = nullptr;
        bool alphaTested = false;
    };

    // Up to this many lights reach the shader in one frame. The world has 588
    // and their ranges are 3 to 40 metres, so nowhere sees many at once.
    static constexpr std::uint32_t c_MaxFrameLights = 16;

    std::vector<genome::PointLight> m_lights;
    std::vector<std::uint32_t> m_lightmapColours;
    std::vector<float> m_lightmapIncident;
    std::vector<float> m_lightmapCoords;
    const genome::Image *m_lightmapAtlas = nullptr;
    Texture m_lightmapTexture{};
    GpuArena m_coordArena;
    GpuArena m_lightmapArena;
    GpuArena m_incidentArena;
    std::size_t m_litLights = 0;
    Buffer m_lightBuffer[Device::c_FramesInFlight]{};
    VkDescriptorSetLayout m_lightLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_lightSet[Device::c_FramesInFlight]{};
    VkDescriptorPool m_lightPool = VK_NULL_HANDLE;

    GpuContext m_gpu = nullptr;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;

    GpuArena m_vertices;
    GpuArena m_indices;
    ArenaUploader m_uploader;
    Budget m_budget;
    Buffer m_instanceBuffer[Device::c_FramesInFlight]{};
    std::size_t m_vertexCount = 0;
    std::size_t m_indexCount = 0;
    std::size_t m_instanceCount = 0;
    std::size_t m_rangeCount = 0;
    std::size_t m_bakedInstances = 0;
    // Where addSector's time goes. Building a mesh's arrays and rebuilding the
    // derived views are both plain CPU work sitting on the render thread, and
    // whether they dominate decides what is worth moving.
    double m_secondsPlacing = 0.0, m_secondsRebuilding = 0.0, m_secondsFlushing = 0.0;

    // Where a mesh sits in the arenas, and how many batches are relying on it.
    // Meshes are shared between sectors - a crate is a crate everywhere - so
    // the geometry is uploaded once and only given back when the last batch
    // using it goes.
    struct MeshGeometry
    {
        std::size_t refs = 0;
        std::size_t vertexOffset = 0, vertexCount = 0;
        std::size_t indexOffset = 0, indexCount = 0;

        // One per drawable element, already rebased onto the arena.
        struct Element
        {
            std::uint32_t firstIndex = 0;
            std::uint32_t indexCount = 0;
            std::int32_t vertexOffset = 0;
        };
        std::vector<Element> elements;
    };
    std::map<const genome::Mesh *, MeshGeometry> m_geometry;
    bool placeMesh(Device &device, const genome::Mesh &mesh, MeshGeometry *&out, std::string *error);
    void releaseMesh(Device &device, const genome::Mesh *mesh);

    // Ranges a departed sector gave back. They cannot go on the free list at
    // once: a frame already submitted may still be drawing from them. Until
    // uploads stopped draining the queue this was covered by accident.
    struct PendingRelease
    {
        GpuArena *arena = nullptr;
        std::size_t offset = 0, count = 0;
        std::uint64_t frame = 0;
    };
    std::vector<PendingRelease> m_pendingReleases;
    std::uint64_t m_frameCounter = 0;
    void queueRelease(GpuArena &arena, std::size_t offset, std::size_t count);

    // One batch per mesh, whichever sectors placed it.
    std::map<const genome::Mesh *, std::size_t> m_batchOf;

    // What a sector is holding, so that giving it back is one step.
    struct Sector
    {
        std::uint32_t id = 0;
        std::size_t colourBase = 0;
        std::size_t colourCount = 0;
    };
    std::vector<Sector> m_sectors;

    // Extents, occluders, the grid and the mesh-to-batch map, all of which are
    // views of the batch list. Marked stale when it changes and rebuilt once,
    // before the cull that reads them - a burst of departures would otherwise
    // pay for a full rebuild each.
    void rebuildDerived();
    void ensureDerived();
    bool m_derivedStale = false;
    // The grid says its size once. Saying it on every rebuild means saying it
    // in the middle of a frame, to an unbuffered stdout.
    bool m_reportedGrid = false;
    std::size_t m_staleArrivals = 0;
    std::size_t m_staleLookups = 0;
    std::size_t m_staleOutOfRange = 0;

    // One entry per texture, keyed by the image it was made from - which is
    // the route that did not exist before, and without which nothing could find
    // a texture to free. The count is of batches naming it, not sectors: a
    // batch is keyed by mesh and carries several sectors' transforms at once.
    struct TextureEntry
    {
        Texture texture{};
        VkDescriptorSet set = VK_NULL_HANDLE;
        std::size_t refs = 0;
    };
    std::map<const genome::Image *, TextureEntry> m_textureOf;
    VkDescriptorSet m_whiteSet = VK_NULL_HANDLE;

    // Takes a reference, making the texture if this is the first. Null means
    // the shared white one, which is never counted and never freed.
    VkDescriptorSet acquireTexture(Device &device, const genome::Image *image, std::string *error);
    void releaseTexture(const genome::Image *image);

    // Handles a batch gave back. They cannot be destroyed at once: a submitted
    // frame may still be binding that set, which is
    // VUID-vkFreeDescriptorSets-pDescriptorSets-00309.
    struct RetiringTexture
    {
        Texture texture{};
        VkDescriptorSet set = VK_NULL_HANDLE;
        std::uint64_t frame = 0;
    };
    // Destroying a texture is four driver calls, and a burst of departures
    // makes a burst of them come due together. Nothing waits on one being
    // destroyed promptly - it has already waited out the frames in flight - so
    // they go a few a frame.
    static constexpr std::size_t c_RetirePerFrame = 8;
    std::vector<RetiringTexture> m_retiringTextures;
    std::size_t m_texturesFreed = 0;
    std::size_t m_texturesRemade = 0;
    std::size_t m_worstRetireBurst = 0;
    std::set<const genome::Image *> m_everFreed;

    // Per batch: the transforms and their bounds, kept so each frame can pick
    // the visible subset.
    struct Batch
    {
        std::vector<genome::WorldMatrix> transforms;
        std::vector<std::array<float, 6>> bounds;
        // The draws this batch makes, by value: a batch that leaves takes its
        // draws with it, which an index into a shared list cannot do.
        std::vector<Range> ranges;
        const genome::Mesh *mesh = nullptr;

        // Which sector put each instance here, in step with the transforms. A
        // sector leaving compacts these lists; nothing on the card moves,
        // because the instance buffer is rewritten by the cull every frame
        // regardless.
        std::vector<std::uint32_t> sectorOf;

        // Where the mesh starts in the vertex arena, needed to rebase an
        // instance's lighting, and its object-space box, needed to work out the
        // batch's extent again after instances have gone.
        std::int32_t meshFirstVertex = 0;
        std::array<float, 6> localBox{};
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
    std::size_t m_foliageSkipped = 0;

    VkDescriptorSetLayout m_descriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    Texture m_white;

    std::array<float, 3> m_boundsMin{};
    std::array<float, 3> m_boundsMax{};
};

} // namespace render
