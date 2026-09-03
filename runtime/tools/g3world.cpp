// Draws the landscape. The low-poly tiles already carry world coordinates, so
// this needs no placement data: load them all and fly around.
//
//   g3world <_compiledMesh.pak> [name filter]
//
// Hold the right mouse button to look, WASD to move, Q/E to drop and rise,
// Shift to go faster, Ctrl to creep, O to toggle occlusion culling.

#include "genome/image.h"
#include "genome/material.h"
#include "genome/mesh.h"
#include "genome/pak.h"
#include "genome/residency.h"
#include "genome/spt.h"
#include "genome/tree.h"
#include "genome/lightmap.h"
#include "genome/world.h"
#include "render/window.h"
#include "render/profile.h"
#include "render/tree_atlas.h"
#include "render/world_renderer.h"

// windows.h is here only for the virtual-key codes, and its min/max macros
// would shadow the standard ones.
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <set>

namespace
{

std::array<float, 16> lookAt(const std::array<float, 3> &eye, const std::array<float, 3> &target)
{
    std::array<float, 3> forward{target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    const float length = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    for (float &value : forward)
        value /= length;

    const std::array<float, 3> worldUp{0.0f, 1.0f, 0.0f};
    std::array<float, 3> right{forward[1] * worldUp[2] - forward[2] * worldUp[1],
                               forward[2] * worldUp[0] - forward[0] * worldUp[2],
                               forward[0] * worldUp[1] - forward[1] * worldUp[0]};
    const float rightLength = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    for (float &value : right)
        value /= rightLength;

    const std::array<float, 3> up{right[1] * forward[2] - right[2] * forward[1],
                                  right[2] * forward[0] - right[0] * forward[2],
                                  right[0] * forward[1] - right[1] * forward[0]};

    return {right[0],
            up[0],
            -forward[0],
            0.0f,
            right[1],
            up[1],
            -forward[1],
            0.0f,
            right[2],
            up[2],
            -forward[2],
            0.0f,
            -(right[0] * eye[0] + right[1] * eye[1] + right[2] * eye[2]),
            -(up[0] * eye[0] + up[1] * eye[1] + up[2] * eye[2]),
            (forward[0] * eye[0] + forward[1] * eye[1] + forward[2] * eye[2]),
            1.0f};
}

std::array<float, 16> perspective(float fovRadians, float aspect, float nearPlane, float farPlane)
{
    const float f = 1.0f / std::tan(fovRadians * 0.5f);
    std::array<float, 16> m{};
    m[0] = f / aspect;
    m[5] = -f;
    m[10] = farPlane / (nearPlane - farPlane);
    m[11] = -1.0f;
    m[14] = (nearPlane * farPlane) / (nearPlane - farPlane);
    return m;
}

std::array<float, 16> multiply(const std::array<float, 16> &a, const std::array<float, 16> &b)
{
    std::array<float, 16> out{};
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
                sum += a[k * 4 + row] * b[column * 4 + k];
            out[column * 4 + row] = sum;
        }
    return out;
}

// Somewhere to put the baked patches. They are small - the whole game's come to
// about six and a half million texels - so one image holds a sector's easily,
// and a shelf packer is enough for rectangles that arrive in no particular size.
struct PatchAtlas
{
    // 4096 holds a resident set of sectors; 2048 did not, refusing 11368 of
    // 19046 patches at sixteen cells.
    static constexpr std::uint32_t c_Size = 4096;

    // Shelf packing laid the resident set's patches - which cover 14.7% of the
    // atlas - across 3420 of its 4096 rows: five and a half times the room they
    // need, because a shelf is as tall as the tallest thing on it and runs the
    // full width. A tile wastes only inside itself. It is also the unit a
    // sector can give back when it leaves, which a shelf never was.
    //
    // 128 because a patch is nearly always smaller: of the 203884 patches in
    // the game 1377 are wider than 64, 35 wider than 128 and 3 wider than 256.
    // The rare big one takes a rectangle of tiles.
    static constexpr std::uint32_t c_Tile = 128;
    static constexpr std::uint32_t c_Grid = c_Size / c_Tile;

    genome::Image image;
    std::vector<char> taken = std::vector<char>(std::size_t(c_Grid) * c_Grid, 0);
    std::size_t tilesTaken = 0;
    // Which tiles the sector being loaded has taken, so it can give them back.
    std::vector<std::uint32_t> sectorTiles;
    std::size_t packed = 0, refused = 0;
    std::size_t texels = 0, widest = 0, tallest = 0;

    // The tile currently being filled, shelf by shelf inside its 128 squares.
    bool hasTile = false;
    std::uint32_t tileX = 0, tileY = 0, penX = 0, shelfY = 0, shelfHeight = 0;

    PatchAtlas()
    {
        image.width = c_Size;
        image.height = c_Size;
        image.faceCount = 1;
        image.format = genome::ImageFormat::A8R8G8B8;
        image.data.assign(std::size_t(c_Size) * c_Size * 4, 0);
        image.levels.push_back({c_Size, c_Size, 0, std::uint32_t(image.data.size())});
        image.faceStride = std::uint32_t(image.data.size());
    }

    // A free rectangle of tiles, top-left first. One tile is the common case
    // and the scan finds it immediately.
    bool takeTiles(std::uint32_t wide, std::uint32_t tall, std::uint32_t &outX, std::uint32_t &outY)
    {
        if (wide > c_Grid || tall > c_Grid)
            return false;
        for (std::uint32_t y = 0; y + tall <= c_Grid; ++y)
            for (std::uint32_t x = 0; x + wide <= c_Grid; ++x)
            {
                bool free = true;
                for (std::uint32_t dy = 0; dy < tall && free; ++dy)
                    for (std::uint32_t dx = 0; dx < wide && free; ++dx)
                        free = taken[std::size_t(y + dy) * c_Grid + x + dx] == 0;
                if (!free)
                    continue;
                for (std::uint32_t dy = 0; dy < tall; ++dy)
                    for (std::uint32_t dx = 0; dx < wide; ++dx)
                    {
                        taken[std::size_t(y + dy) * c_Grid + x + dx] = 1;
                        sectorTiles.push_back((y + dy) * c_Grid + x + dx);
                    }
                tilesTaken += std::size_t(wide) * tall;
                outX = x;
                outY = y;
                return true;
            }
        return false;
    }

    // A sector starting to pack must not continue in a tile another sector
    // owns: its patches would never be uploaded, because the tile is not in its
    // own list, and the tile would be freed under it when that sector left.
    void beginSector()
    {
        sectorTiles.clear();
        hasTile = false;
    }

    // A sector leaving hands its tiles back. Nothing is cleared: the
    // coordinates that pointed at them left with it.
    void freeTiles(const std::vector<std::uint32_t> &tiles)
    {
        for (std::uint32_t tile : tiles)
            if (tile < taken.size() && taken[tile])
            {
                taken[tile] = 0;
                --tilesTaken;
            }
        hasTile = false;
    }

    void blit(const genome::LightmapBitmap &bitmap, std::uint32_t x, std::uint32_t y)
    {
        const std::uint32_t width = std::uint32_t(bitmap.width), height = std::uint32_t(bitmap.height);
        for (std::uint32_t row = 0; row < height; ++row)
            std::memcpy(&image.data[(std::size_t(y + row) * c_Size + x) * 4],
                        &bitmap.data[std::size_t(row) * width * 4], std::size_t(width) * 4);
        texels += std::size_t(width) * height;
        widest = std::max(widest, std::size_t(width));
        tallest = std::max(tallest, std::size_t(height));
        ++packed;
    }

    // Returns false when the atlas is full; the caller then leaves that patch
    // unlit rather than drawing someone else's light.
    bool place(const genome::LightmapBitmap &bitmap, std::uint32_t &outX, std::uint32_t &outY)
    {
        const std::uint32_t width = std::uint32_t(bitmap.width), height = std::uint32_t(bitmap.height);
        if (width == 0 || height == 0 || width > c_Size || height > c_Size)
            return false;

        if (width > c_Tile || height > c_Tile)
        {
            // Its own rectangle of tiles, and no shelf kept: there are 35 of
            // these in the game and chasing the offcuts is not worth it.
            std::uint32_t tx = 0, ty = 0;
            if (!takeTiles((width + c_Tile - 1) / c_Tile, (height + c_Tile - 1) / c_Tile, tx, ty))
            {
                ++refused;
                return false;
            }
            outX = tx * c_Tile;
            outY = ty * c_Tile;
            blit(bitmap, outX, outY);
            return true;
        }

        for (int attempt = 0; attempt < 2; ++attempt)
        {
            if (!hasTile)
            {
                if (!takeTiles(1, 1, tileX, tileY))
                {
                    ++refused;
                    return false;
                }
                hasTile = true;
                penX = shelfY = shelfHeight = 0;
            }

            if (penX + width > c_Tile)
            {
                penX = 0;
                shelfY += shelfHeight;
                shelfHeight = 0;
            }
            if (shelfY + height > c_Tile)
            {
                // This tile is full; the next patch starts a new one.
                hasTile = false;
                continue;
            }

            outX = tileX * c_Tile + penX;
            outY = tileY * c_Tile + shelfY;
            blit(bitmap, outX, outY);
            penX += width;
            shelfHeight = std::max(shelfHeight, height);
            return true;
        }
        ++refused;
        return false;
    }
};

// Everything one sector contributes, kept together so it can be handed over and
// taken back as a unit. Meshes it owns are the ones nobody else can be using -
// its own scattered vegetation; the placed meshes and the grown trees are
// shared and outlive it.
struct SectorContent
{
    std::string name;
    std::uint32_t id = 0;
    std::vector<render::MeshInstances> batches;
    render::WorldRenderer::SectorLighting lighting;
    std::vector<genome::PointLight> lights;
    std::vector<std::unique_ptr<genome::Mesh>> meshes;
    // Which of its batches is the full-detail form of which tree, so that a
    // billboard can be given the same instances once the atlas is baked.
    std::vector<std::pair<std::string, std::size_t>> treeSlots;
    // The atlas tiles its baked patches went into.
    std::vector<std::uint32_t> tiles;
};

// What the loader hands back. The tile pixels travel with it because the atlas
// belongs to the loader's thread and the main thread must not read from it.
struct LoadedSector
{
    SectorContent content;
    std::vector<std::uint8_t> tilePixels;
    bool ok = false;
};

// One thread owns every archive and every cache a load touches, and nothing
// else calls in. There is no version of this that shares them: PakArchive::read
// seeks a FILE* it holds, and the caches are plain maps. The main thread posts
// a path and takes back a finished sector.
class SectorLoader
{
  public:
    using Load = std::function<void(const genome::PakEntry &, std::uint32_t, LoadedSector &)>;
    using GiveBack = std::function<void(const std::vector<std::uint32_t> &)>;

    void start(Load load, GiveBack giveBack)
    {
        m_load = std::move(load);
        m_giveBack = std::move(giveBack);
        m_thread = std::thread([this] { run(); });
    }

    void stop()
    {
        if (!m_thread.joinable())
            return;
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_stopping = true;
        }
        m_wake.notify_all();
        m_thread.join();
    }

    // True while a sector is queued or being loaded. One at a time: the point
    // is to keep the frame free, not to load faster.
    bool busy() const
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        return m_working || !m_queue.empty();
    }

    void request(const genome::PakEntry &entry, std::uint32_t id)
    {
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_queue.push_back({&entry, id});
        }
        m_wake.notify_one();
    }

    // Tiles a departed sector gave back. Freed on the loader's thread, since
    // the atlas is its own.
    void giveBackTiles(std::vector<std::uint32_t> tiles)
    {
        if (tiles.empty())
            return;
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            m_returning.push_back(std::move(tiles));
        }
        m_wake.notify_one();
    }

    bool take(LoadedSector &out)
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (m_done.empty())
            return false;
        out = std::move(m_done.front());
        m_done.pop_front();
        return true;
    }

  private:
    struct Request
    {
        const genome::PakEntry *entry = nullptr;
        std::uint32_t id = 0;
    };

    void run()
    {
        for (;;)
        {
            Request request;
            std::vector<std::uint32_t> returning;
            {
                std::unique_lock<std::mutex> guard(m_mutex);
                m_wake.wait(guard, [this] { return m_stopping || !m_queue.empty() || !m_returning.empty(); });
                if (m_stopping && m_queue.empty() && m_returning.empty())
                    return;
                if (!m_returning.empty())
                {
                    returning = std::move(m_returning.front());
                    m_returning.pop_front();
                }
                else
                {
                    request = m_queue.front();
                    m_queue.pop_front();
                    m_working = true;
                }
            }

            if (!returning.empty())
            {
                m_giveBack(returning);
                continue;
            }

            LoadedSector loaded;
            m_load(*request.entry, request.id, loaded);

            {
                std::lock_guard<std::mutex> guard(m_mutex);
                m_done.push_back(std::move(loaded));
                m_working = false;
            }
        }
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::thread m_thread;
    std::deque<Request> m_queue;
    std::deque<LoadedSector> m_done;
    std::deque<std::vector<std::uint32_t>> m_returning;
    bool m_working = false;
    bool m_stopping = false;
    Load m_load;
    GiveBack m_giveBack;
};

} // namespace

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    if (argc < 2)
    {
        std::puts("usage: g3world <_compiledMesh.pak> [mesh filter] [--sectors <Projects_compiled.pak> [sector "
              "filter]] [--shot <out.ppm>]");
        return 2;
    }

    const bool hasFilter = argc > 2 && argv[2][0] != 45;
    const std::string filter = hasFilter ? argv[2] : "g3_world_lowpoly_landscape_01/";

    // The mesh archive sits in the game's Data folder, so its siblings are found
    // beside it rather than asked for separately.
    std::string dataDirectory = argv[1];
    const std::size_t lastSlash = dataDirectory.find_last_of("/\\");
    dataDirectory = lastSlash == std::string::npos ? std::string(".") : dataDirectory.substr(0, lastSlash);

    std::string error;
    const auto archive = genome::PakArchive::open(argv[1], &error);
    if (!archive)
    {
        std::cerr << "error: " << error << "\n";
        return 1;
    }

    std::vector<std::unique_ptr<genome::Image>> images;

    // A batch is one mesh and every transform it is placed with. Landscape
    // tiles are already in world space, so they get a single identity instance.
    std::vector<std::unique_ptr<genome::Mesh>> ownedMeshes;
    std::map<std::string, std::size_t> batchOf;
    std::vector<render::MeshInstances> batches;
    // Sectors, loaded one at a time so that one at a time is also how they can
    // leave. Everything a later arrival needs lives out here with them: the
    // archive, the caches, the residency index and the loader itself, because
    // an arrival takes exactly the same path as the startup load.
    std::vector<SectorContent> residentSectors;
    std::function<bool(const genome::PakEntry &, SectorContent &)> loadSector;
    std::unique_ptr<genome::PakArchive> world;
    std::map<std::string, genome::SectorBounds> boundsOf;
    std::string sectorFilter = "_cstat.node";
    // A sector's box is filed under its path without the extension.
    const auto sectorKey = [](const std::string &path) {
        const std::size_t dot = path.find_last_of('.');
        return dot == std::string::npos ? path : path.substr(0, dot);
    };
    genome::ResidentCells heldCells;
    std::array<float, 3> residencyEye{};
    std::uint32_t nextSectorId = 1;
    std::map<std::string, genome::Mesh *> meshOf;

    // Tree kinds in the order they were grown - the billboard atlas is baked
    // from exactly these, and a cell is found by a kind's place in this list.
    std::vector<std::string> treeKinds;
    std::map<std::string, std::array<genome::Mesh *, 2>> treeMeshOf;
    std::vector<genome::PointLight> worldLights;
    // Every instance's baked vertex lighting, end to end. The shader indexes it
    // by the base each instance carries, which is how per-instance lighting
    // survives sharing one vertex buffer between instances.
    std::vector<std::uint32_t> lightmapColours;
    std::vector<float> lightmapIncident;
    std::vector<float> lightmapCoords;
    PatchAtlas patchAtlas;
    std::size_t patchedVertices = 0, sizeMatches = 0, sizeMismatches = 0;
    std::unique_ptr<genome::PakArchive> lightmapArchive;
    // Where the loading time actually goes, so streaming is designed against
    // numbers rather than against a guess about which part is slow.
    double timeReadingSectors = 0.0, timeMeshes = 0.0, timeLightmaps = 0.0, timeTrees = 0.0;
    double timeTextures = 0.0, timeRenderer = 0.0;
    const auto now = [] { return std::chrono::steady_clock::now(); };
    const auto since = [](std::chrono::steady_clock::time_point start) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    };
    std::size_t lightmapsFound = 0, lightmapsMissing = 0;
    static const genome::WorldMatrix c_Identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    const bool wantLandscape = filter != "none";
    std::size_t failed = 0;
    if (wantLandscape)
    {
        for (const genome::PakEntry &entry : archive->entries())
        {
            if (entry.deleted || entry.path.find(filter) == std::string::npos)
                continue;
            if (entry.path.size() < 6 || entry.path.compare(entry.path.size() - 6, 6, ".xcmsh") != 0)
                continue;

            auto mesh = std::make_unique<genome::Mesh>();
            if (!genome::loadMesh(archive->read(entry, &error), *mesh, &error))
            {
                ++failed;
                continue;
            }

            render::MeshInstances batch;
            batch.mesh = mesh.get();
            batch.transforms.push_back(c_Identity);
            batches.push_back(std::move(batch));
            ownedMeshes.push_back(std::move(mesh));
        }
    }

    int sectorArgument = 0;
    const char *shotPath = nullptr;
    // Everything so far has been counted in objects. This counts milliseconds.
    int benchFrames = 0;
    int cameraArgument = 0;
    int treeArgument = 0;
    bool validation = false;
    // Ask the swapchain not to wait for the display, so a benchmark measures
    // this program instead of the refresh interval.
    bool uncapped = false;
    // On, and the reason is worth keeping because it reversed once. While the
    // cull was serial it cost 1.24 ms to save 0.54M triangles on a card that
    // was never waited for, and it was switched off. Making the cull parallel
    // moved the bottleneck onto the card, which is the condition that was
    // written down beside this switch at the time - and it fired:
    //
    //              cull   fence wait   frame (median)
    //     off      0.18       0.66          1.05 ms
    //     on       0.49       0.15          0.92 ms
    //
    // It now costs 0.31 ms spread over eight threads and saves half a
    // millisecond of waiting for the card. The same feature, the same numbers,
    // opposite answers - because what the frame is waiting for changed.
    bool startWithOcclusion = true;
    // Below this many pixels an instance is not asked whether it is hidden.
    float occlusionPixels = 0.0f;
    // Nought draws with the interpolated normal, one with the mapped one.
    float normalStrength = 1.0f;
    // Runs the cull several times a frame. The cost of the extra passes is the
    // warm-cache cost; the difference from the first is what the memory costs.
    int cullRepeat = 1;
    // The card is the bottleneck now, so the pixel count is a first-class
    // parameter rather than a constant.
    int windowWidth = 1280, windowHeight = 720;
    // Threads the cull runs on, the caller included. Zero leaves the default.
    int cullThreads = 0;
    // Shakes the timing inside the cull so two runs never line up. For
    // comparisons only; it costs whatever it is set to.
    int cullJitter = 0;
    int cullGrain = 0;
    // Residency is decided against a far plane of its own, not the one the
    // camera draws with: it is the game's number, and what made 36 sectors the
    // answer rather than some other count.
    constexpr float c_ResidencyFar = 10000.0f;
    // Fly forward at this many units a second, so that arrivals and departures
    // can be measured without a hand on the keyboard.
    float flySpeed = 0.0f;
    // The angles above are degrees for a person; --radians takes them as the
    // shot line prints them, so a flight can be reproduced exactly.
    bool cameraInRadians = false;
    // Load only what is close enough to matter, the way the game does: a
    // rectangle of 10000-unit cells around the camera. Off by default so the
    // whole-world runs still work.
    bool streaming = false;
    // How much room the arenas get beyond what the first resident set needs.
    // Below 1 the arenas cannot hold what arrives and addSector starts
    // refusing sectors, which is the only way to exercise its unwind.
    double budgetRoom = 0.0;
    // A baked patch stands in for the daylight on the surface it covers, rather
    // than adding to it: the bake already accounted for the sun that reached
    // there, and adding both counts the same light twice. Measured - replacing
    // gives contrast 25.4 against 23.1 - and --baked-adds puts it back for
    // comparison.
    float lightmapReplaces = 1.0f;
    // How many different trees are grown per definition before they repeat.
    constexpr std::uint32_t c_TreeVariants = 3;
    // Where a tree drops to its thinned form, in world units - a metre is a
    // hundred, so this is sixty metres.
    float treeLodDistance = 6000.0f;
    // And where it becomes a single quad. Four hundred metres.
    float treeBillboardDistance = 40000.0f;
    for (int index = 2; index < argc; ++index)
    {
        const bool hasValue = index + 1 < argc;
        if (std::string(argv[index]) == "--sectors" && hasValue)
            sectorArgument = index + 1;
        if (std::string(argv[index]) == "--shot" && hasValue)
            shotPath = argv[index + 1];
        if (std::string(argv[index]) == "--lod" && hasValue)
            treeLodDistance = float(std::atof(argv[index + 1]));
        if (std::string(argv[index]) == "--billboard" && hasValue)
            treeBillboardDistance = float(std::atof(argv[index + 1]));
        if (std::string(argv[index]) == "--bench" && hasValue)
            benchFrames = std::atoi(argv[index + 1]);
        if (std::string(argv[index]) == "--camera" && index + 5 < argc)
            cameraArgument = index + 1;
        if (std::string(argv[index]) == "--radians")
            cameraInRadians = true;
        if (std::string(argv[index]) == "--tree" && hasValue)
            treeArgument = index + 1;
        if (std::string(argv[index]) == "--validate")
            validation = true;
        if (std::string(argv[index]) == "--uncapped")
            uncapped = true;
        if (std::string(argv[index]) == "--no-occlusion")
            startWithOcclusion = false;
        if (std::string(argv[index]) == "--size" && index + 2 < argc)
        {
            windowWidth = std::atoi(argv[index + 1]);
            windowHeight = std::atoi(argv[index + 2]);
        }
        if (std::string(argv[index]) == "--threads" && hasValue)
            cullThreads = std::atoi(argv[index + 1]);
        if (std::string(argv[index]) == "--cull-grain" && hasValue)
            cullGrain = std::atoi(argv[index + 1]);
        if (std::string(argv[index]) == "--cull-jitter" && hasValue)
            cullJitter = std::atoi(argv[index + 1]);
        if (std::string(argv[index]) == "--cull-repeat" && hasValue)
            cullRepeat = std::max(1, std::atoi(argv[index + 1]));
        if (std::string(argv[index]) == "--normal-strength" && hasValue)
            normalStrength = float(std::atof(argv[index + 1]));
        if (std::string(argv[index]) == "--occlusion-pixels" && hasValue)
            occlusionPixels = float(std::atof(argv[index + 1]));
        if (std::string(argv[index]) == "--stream")
            streaming = true;
        if (std::string(argv[index]) == "--squeeze" && hasValue)
            budgetRoom = std::atof(argv[index + 1]);
        if (std::string(argv[index]) == "--fly" && hasValue)
            flySpeed = float(std::atof(argv[index + 1]));
        if (std::string(argv[index]) == "--baked-adds")
            lightmapReplaces = 0.0f;
    }

    const bool showOneTree = treeArgument != 0 && treeArgument + 1 < argc &&
                             std::string(argv[treeArgument + 1]).find(".spt") != std::string::npos;
    if (showOneTree)
    {
        const auto trees = genome::PakArchive::open(argv[treeArgument], nullptr);
        genome::SpeedTree definition;
        if (!trees || !genome::loadSpeedTree(trees->read(argv[treeArgument + 1], &error), definition, &error))
            std::printf("warning: could not read %s: %s\n", argv[treeArgument + 1], error.c_str());
        else
        {
            // A row of them, so the variance between instances is visible rather
            // than asserted.
            for (std::uint32_t index = 0; index < 5; ++index)
            {
                auto mesh = std::make_unique<genome::Mesh>();
                if (!genome::growTree(definition, definition.seed + index * 7919u, genome::TreeGrowth{}, *mesh))
                    continue;

                render::MeshInstances batch;
                batch.mesh = mesh.get();
                batch.occludes = false;
                genome::WorldMatrix world{};
                world[0] = world[5] = world[10] = world[15] = 1.0f;
                world[12] = float(index) * (mesh->boundsMax[0] - mesh->boundsMin[0] + 200.0f);
                batch.transforms.push_back(world);
                batches.push_back(std::move(batch));
                ownedMeshes.push_back(std::move(mesh));
            }
            std::printf("grew %zu trees from %s\n", batches.size(), argv[treeArgument + 1]);
        }
    }

    lightmapArchive = genome::PakArchive::open(dataDirectory + "/Lightmaps.pak", nullptr);

    // Sectors name their trees by definition; growing one mesh per definition
    // and instancing it is the only way 57315 of them fit, and it is what the
    // game itself did - it batched every tree of a species from one buffer.
    std::unique_ptr<genome::PakArchive> treeArchive;
    std::map<std::string, std::string> treePathOf;
    if (treeArgument != 0)
    {
        treeArchive = genome::PakArchive::open(argv[treeArgument], nullptr);
        if (treeArchive)
            for (const genome::PakEntry &entry : treeArchive->entries())
            {
                if (entry.deleted)
                    continue;
                const std::size_t slash = entry.path.find_last_of('/');
                std::string name = slash == std::string::npos ? entry.path : entry.path.substr(slash + 1);
                treePathOf.emplace(name, entry.path);
            }
    }

    if (sectorArgument != 0)
    {
        world = genome::PakArchive::open(argv[sectorArgument], nullptr);
        if (!world)
            std::puts("warning: could not open the world archive");
        else
        {
            sectorFilter = sectorArgument + 1 < argc ? argv[sectorArgument + 1] : "_cstat.node";
            std::size_t placed = 0, sectors = 0, missing = 0, grass = 0, planted = 0, missingTrees = 0;


            // The residency index: every sector's own bounding box, which is
            // 2177 files of 197 bytes rather than the 347 MB they describe. The
            // box is what decides, not the cell in the file name - the box of
            // x55000z55000 starts a whole cell further out, at 45000.
            if (streaming)
            {
                const auto indexStart = now();
                for (const genome::PakEntry &entry : world->entries())
                {
                    if (entry.deleted || entry.path.find("_cstat.lrgeodat") == std::string::npos)
                        continue;
                    std::string ignored;
                    genome::SectorBounds bounds;
                    if (!genome::loadSectorBounds(world->read(entry, &ignored), bounds))
                        continue;
                    std::string key = entry.path;
                    const std::size_t dot = key.find_last_of('.');
                    if (dot != std::string::npos)
                        key = key.substr(0, dot);
                    boundsOf.emplace(key, bounds);
                }
                std::printf("residency index: %zu sector boxes in %.2fs\n", boundsOf.size(), since(indexStart));
            }

            // Where the camera will be, since that is what decides residency.
            // With no camera given, the middle of the map.
            std::array<float, 3> eye{55000.0f, 8000.0f, 55000.0f};
            if (cameraArgument != 0)
                eye = {float(std::atof(argv[cameraArgument])), float(std::atof(argv[cameraArgument + 1])),
                       float(std::atof(argv[cameraArgument + 2]))};

            const genome::ResidentCells resident = genome::residentCells(eye, c_ResidencyFar);
            heldCells = resident;
            residencyEye = eye;
            if (streaming)
                std::printf("resident cells x %d..%d, z %d..%d around (%.0f, %.0f)\n", resident.left,
                            resident.right, resident.top, resident.bottom, eye[0], eye[2]);

            std::size_t skipped = 0;

            // One sector's worth of loading. Everything it appends to goes into
            // `out`, including the two maps that used to span the whole world:
            // a mesh placed in two sectors now makes an entry in each, and the
            // renderer merges them back into one batch by mesh pointer, which
            // is what keeps the draw count where it was.
            loadSector = [&](const genome::PakEntry &entry, SectorContent &out) -> bool {
                auto &batches = out.batches;
                auto &lightmapColours = out.lighting.colours;
                auto &lightmapIncident = out.lighting.incident;
                auto &lightmapCoords = out.lighting.coords;
                auto &worldLights = out.lights;
                std::map<std::string, std::size_t> batchOf;
                std::map<std::string, std::size_t> treeBatchOf;

                genome::WorldLayer layer;
                std::string ignored;
                const auto sectorStart = now();
                const bool read = genome::loadWorldNode(world->read(entry, &ignored), layer, &ignored);
                timeReadingSectors += since(sectorStart);
                if (!read)
                    return false;

                for (const genome::Placement &placement : layer.placements)
                {
                    if (placement.meshName.empty())
                        continue;

                    // The baked lighting of this instance, attached once its
                    // mesh is known: the charts that address the patches are in
                    // the mesh, not in the lightmap.
                    const auto attachLightmap = [&](const genome::Mesh *lit) {
                        const auto lightmapStart = now();
                        struct Timed
                        {
                            double &into;
                            std::chrono::steady_clock::time_point start;
                            ~Timed()
                            {
                                into += std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
                                            .count();
                            }
                        } timed{timeLightmaps, lightmapStart};

        // The baked lighting of this instance, found by the mesh it
        // places and its own identifier - which is exactly how the
        // archive names its files.
        std::int32_t lightmapBase = -1;
        if (lightmapArchive && !placement.guid.empty())
        {
            std::string name = placement.meshName;
            const std::size_t dot = name.find_last_of('.');
            if (dot != std::string::npos)
                name = name.substr(0, dot);
            name += "_{" + placement.guid + "}.xlmp";

            std::string why;
            genome::Lightmap map;
            const std::vector<std::uint8_t> bytes = lightmapArchive->read(name, &why);
            if (!bytes.empty() && genome::loadLightmap(bytes, map, &why) && !map.elements.empty() &&
                !map.elements.front().colours.empty())
            {
                lightmapBase = std::int32_t(lightmapColours.size());
                const std::size_t coordBase = lightmapColours.size();
                std::size_t elementIndex = 0, vertexRunning = 0;
                for (const genome::LightmapElement &element : map.elements)
                {
                    lightmapColours.insert(lightmapColours.end(), element.colours.begin(),
                                           element.colours.end());
                    lightmapCoords.resize(lightmapColours.size() * 2, -1.0f);

                    // The charts live in the mesh and the patches in
                    // the lightmap, one for one. A chart's extent is
                    // in world units and so are its coordinates, so
                    // the texel is 1 + uv * scaling - the one being
                    // the gutter the bake leaves on every side.
                    const genome::MeshElement *meshElement =
                        lit && elementIndex < lit->elements.size() ? &lit->elements[elementIndex]
                                                                  : nullptr;
                    if (meshElement && !meshElement->lightmapUV.empty())
                    {
                        for (std::size_t chart = 0;
                             chart < meshElement->charts.size() && chart < element.bitmaps.size();
                             ++chart)
                        {
                            const genome::LightmapBitmap &bitmap = element.bitmaps[chart];
                            if (bitmap.data.empty())
                                continue;

                            const std::array<float, 2> &extent = meshElement->charts[chart].extent;
                            const int wantedWidth =
                                int(std::ceil(extent[0] * map.scaling)) + 2;
                            const int wantedHeight =
                                int(std::ceil(extent[1] * map.scaling)) + 2;
                            if (wantedWidth == bitmap.width && wantedHeight == bitmap.height)
                                ++sizeMatches;
                            else
                                ++sizeMismatches;

                            std::uint32_t atX = 0, atY = 0;
                            if (!patchAtlas.place(bitmap, atX, atY))
                                continue;

                            for (std::uint32_t vertex : meshElement->charts[chart].vertices)
                            {
                                if (vertex >= meshElement->lightmapUV.size())
                                    continue;
                                const std::array<float, 2> &uv = meshElement->lightmapUV[vertex];
                                const std::size_t at = (coordBase + vertexRunning + vertex) * 2;
                                if (at + 1 >= lightmapCoords.size())
                                    continue;
                                lightmapCoords[at] =
                                    (float(atX) + 1.0f + uv[0] * map.scaling) / float(PatchAtlas::c_Size);
                                lightmapCoords[at + 1] =
                                    (float(atY) + 1.0f + uv[1] * map.scaling) / float(PatchAtlas::c_Size);
                                ++patchedVertices;
                            }
                        }
                    }
                    vertexRunning += element.colours.size();
                    ++elementIndex;
                    // The two run in step, so a missing direction
                    // array still has to occupy its place.
                    lightmapIncident.resize(lightmapColours.size() * 3, 0.0f);
                    for (std::size_t index = 0; index < element.incident.size(); ++index)
                        lightmapIncident[lightmapIncident.size() - element.incident.size() + index] =
                            element.incident[index];
                }
                ++lightmapsFound;
            }
            else
                ++lightmapsMissing;
        }

        return lightmapBase;
                    };

                    const auto known = batchOf.find(placement.meshName);
                    if (known != batchOf.end())
                    {
                        if (known->second != std::size_t(-1))
                        {
                            batches[known->second].lightmapBase.push_back(
                                attachLightmap(batches[known->second].mesh));
                            batches[known->second].transforms.push_back(placement.world);
                            batches[known->second].bounds.push_back(
                                {placement.boundsMin[0], placement.boundsMin[1], placement.boundsMin[2],
                                 placement.boundsMax[0], placement.boundsMax[1], placement.boundsMax[2]});
                            ++placed;
                        }
                        continue;
                    }

                    genome::Mesh *shared = nullptr;
                    const auto cached = meshOf.find(placement.meshName);
                    if (cached != meshOf.end())
                        shared = cached->second;
                    else
                    {
                        auto mesh = std::make_unique<genome::Mesh>();
                        const auto meshStart = now();
                        const bool loaded =
                            genome::loadMesh(archive->read(placement.meshName, &ignored), *mesh, &ignored);
                        timeMeshes += since(meshStart);
                        if (loaded)
                        {
                            shared = mesh.get();
                            ownedMeshes.push_back(std::move(mesh));
                        }
                        else
                            ++missing;
                        meshOf.emplace(placement.meshName, shared);
                    }
                    if (!shared)
                    {
                        batchOf.emplace(placement.meshName, std::size_t(-1));
                        continue;
                    }

                    render::MeshInstances batch;
                    batch.mesh = shared;
                    batch.lightmapBase.push_back(attachLightmap(shared));
                    batch.transforms.push_back(placement.world);
                    batch.bounds.push_back({placement.boundsMin[0], placement.boundsMin[1], placement.boundsMin[2],
                                            placement.boundsMax[0], placement.boundsMax[1], placement.boundsMax[2]});
                    batchOf.emplace(placement.meshName, batches.size());
                    batches.push_back(std::move(batch));
                    ++placed;
                }
                worldLights.insert(worldLights.end(), layer.lights.begin(), layer.lights.end());

                for (const genome::TreePlacement &tree : layer.trees)
                {
                    if (!treeArchive)
                        break;

                    // A handful of seeds per definition, so a wood is not one
                    // tree repeated, and the mesh for each is grown once. The
                    // variant comes from where the tree stands rather than from
                    // how many were planted before it: a running count makes
                    // the forest depend on the order sectors happened to be
                    // read, so flying in would grow a different wood from
                    // arriving cold.
                    std::uint32_t scatter = 2166136261u;
                    for (int axis = 12; axis < 15; ++axis)
                    {
                        std::uint32_t bits = 0;
                        std::memcpy(&bits, &tree.world[axis], sizeof(bits));
                        scatter = (scatter ^ bits) * 16777619u;
                    }
                    const std::uint32_t variant = (scatter >> 8) % c_TreeVariants;
                    std::string key = tree.resource;
                    for (char &c : key)
                        c = char(std::tolower(static_cast<unsigned char>(c)));
                    key += char('0' + variant);

                    auto known = treeBatchOf.find(key);
                    if (known == treeBatchOf.end())
                    {
                        std::string path;
                        const auto found = treePathOf.find(key.substr(0, key.size() - 1));
                        if (found != treePathOf.end())
                            path = found->second;

                        std::size_t slot = std::size_t(-1);
                        std::array<genome::Mesh *, 2> grown{nullptr, nullptr};
                        const auto grownAlready = treeMeshOf.find(key);
                        if (grownAlready != treeMeshOf.end())
                            grown = grownAlready->second;
                        else
                        {
                        genome::SpeedTree definition;
                        std::string why;
                        if (!path.empty() &&
                            genome::loadSpeedTree(treeArchive->read(path, &why), definition, &why))
                        {
                            // Two of each: the full tree for close up and a
                            // thinned one past the switch distance. A tree is
                            // four thousand triangles and there are 29138 of
                            // them in view at once, which is where the frame
                            // goes; a distant one covering a few pixels must not
                            // cost the same as one filling the screen.
                            const auto treeStart = now();
                            const std::uint32_t seed = definition.seed + variant * 7919u;
                            for (int level = 0; level < 2; ++level)
                            {
                                genome::TreeGrowth growth;
                                growth.detail = level == 0 ? 1.0f : 0.15f;

                                auto mesh = std::make_unique<genome::Mesh>();
                                if (!genome::growTree(definition, seed, growth, *mesh))
                                    continue;
                                grown[level] = mesh.get();
                                ownedMeshes.push_back(std::move(mesh));
                            }
                            timeTrees += since(treeStart);
                        }
                        else
                            ++missingTrees;

                        // Grown once for the whole world; the billboard atlas
                        // is baked from exactly these, in this order.
                        treeMeshOf.emplace(key, grown);
                        if (grown[0])
                            treeKinds.push_back(key);
                        }

                        if (grown[0])
                        {
                            // Two detail levels: the full tree close up and a
                            // thinned one past the switch distance. A tree is
                            // four thousand triangles and there are 29138 of
                            // them in view at once, which is where the frame
                            // goes; a distant one covering a few pixels must
                            // not cost the same as one filling the screen.
                            slot = batches.size();
                            out.treeSlots.emplace_back(key, slot);
                            for (int level = 0; level < 2; ++level)
                            {
                                if (!grown[level])
                                    break;
                                render::MeshInstances batch;
                                batch.mesh = grown[level];
                                batch.occludes = false;
                                batch.lodNear = level == 0 ? 0.0f : treeLodDistance;
                                batch.lodFar = level == 0 ? treeLodDistance : 0.0f;
                                batches.push_back(std::move(batch));
                            }
                        }
                        known = treeBatchOf.emplace(key, slot).first;
                    }

                    if (known->second == std::size_t(-1))
                        continue;

                    // The sector already knows how big the tree ends up, so its
                    // own bounds decide visibility rather than the grown mesh.
                    // Both detail levels hold every instance; the distance band
                    // decides which one draws it, so nothing is placed twice.
                    const std::array<float, 6> box{tree.boundsMin[0], tree.boundsMin[1], tree.boundsMin[2],
                                                   tree.boundsMax[0], tree.boundsMax[1], tree.boundsMax[2]};
                    for (std::size_t level = 0; level < 2; ++level)
                    {
                        const std::size_t at = known->second + level;
                        if (at >= batches.size())
                            break;
                        batches[at].transforms.push_back(tree.world);
                        batches[at].bounds.push_back(box);
                    }
                    ++planted;
                }

                // Grass is scattered rather than placed: the sector holds one mesh
                // per plant kind, and a grid of instances referring to them.
                const std::size_t firstPlantBatch = batches.size();
                for (const genome::VegetationMesh &plant : layer.vegetationMeshes)
                {
                    auto mesh = std::make_unique<genome::Mesh>();
                    genome::MeshElement element;
                    element.positions = plant.positions;
                    element.normals = plant.normals;
                    element.texCoords = plant.texCoords;
                    element.indices = plant.indices;
                    element.materialName = plant.texture;
                    mesh->elements.push_back(std::move(element));

                    render::MeshInstances batch;
                    batch.mesh = mesh.get();
                    batch.occludes = false;
                    batches.push_back(std::move(batch));
                    out.meshes.push_back(std::move(mesh));
                }

                for (const genome::VegetationInstance &plant : layer.vegetation)
                {
                    if (plant.mesh >= layer.vegetationMeshes.size())
                        continue;
                    render::MeshInstances &batch = batches[firstPlantBatch + plant.mesh];
                    batch.transforms.push_back(plant.world);
                    batch.bounds.push_back({plant.boundsMin[0], plant.boundsMin[1], plant.boundsMin[2],
                                            plant.boundsMax[0], plant.boundsMax[1], plant.boundsMax[2]});
                    ++grass;
                }
                return true;
            };

            for (const genome::PakEntry &entry : world->entries())
            {
                if (entry.deleted || entry.path.find(sectorFilter) == std::string::npos)
                    continue;

                if (streaming)
                {
                    std::string key = entry.path;
                    const std::size_t dot = key.find_last_of('.');
                    if (dot != std::string::npos)
                        key = key.substr(0, dot);
                    const auto box = boundsOf.find(key);
                    if (box == boundsOf.end() || !genome::overlaps(box->second, resident))
                    {
                        ++skipped;
                        continue;
                    }
                }

                SectorContent content;
                content.name = entry.path;
                content.id = nextSectorId++;
                patchAtlas.beginSector();
                if (!loadSector(entry, content))
                    continue;
                content.tiles = std::move(patchAtlas.sectorTiles);
                ++sectors;
                residentSectors.push_back(std::move(content));
            }
            if (streaming)
                std::printf("%zu sectors resident, %zu left on disk\n", sectors, skipped);
            std::printf("%zu sectors, %zu objects placed, %zu meshes missing, %zu plants\n", sectors, placed,
                        missing, grass);
            if (planted != 0 || missingTrees != 0)
                std::printf("%zu trees planted from %zu grown kinds, %zu definitions missing\n", planted,
                            treeKinds.size(), missingTrees);
        }
    }

    if (batches.empty() && residentSectors.empty())
    {
        std::puts("nothing to draw");
        return 1;
    }

    // Every batch there is, flat list and sectors together, for the passes that
    // do not care which sector something came from.
    const auto everyBatch = [&](const std::function<void(std::vector<render::MeshInstances> &)> &visit) {
        visit(batches);
        for (SectorContent &content : residentSectors)
            visit(content.batches);
    };

    // Set up inside the block below, which owns the archives it needs, and used
    // again for every sector that arrives afterwards.
    std::function<void(std::vector<render::MeshInstances> &)> resolveTextures;

    std::size_t vertices = 0, triangles = 0, instances = 0, batchCount = 0;
    std::set<const genome::Mesh *> distinctMeshes;
    everyBatch([&](std::vector<render::MeshInstances> &list) {
        for (const render::MeshInstances &batch : list)
        {
            ++batchCount;
            instances += batch.transforms.size();
            if (!batch.mesh || !distinctMeshes.insert(batch.mesh).second)
                continue;
            vertices += batch.mesh->vertexCount();
            triangles += batch.mesh->triangleCount();
        }
    });
    std::printf("%zu distinct meshes (%zu failed) in %zu sector batches, %zu instances, %zu unique vertices, "
                "%zu unique triangles\n",
                distinctMeshes.size(), failed, batchCount, instances, vertices, triangles);

    // Every mesh element names a material, and those resolve to textures that
    // are shared across the meshes using them. Kept as a pass rather than a
    // phase, because a sector arriving later needs exactly the same work done
    // to it and the caches behind it are what make that cheap.
    const auto materials = genome::PakArchive::open(dataDirectory + "/_compiledMaterial.pak", nullptr);
    const auto imageArchive = genome::PakArchive::open(dataDirectory + "/_compiledImage.pak", nullptr);

    std::set<std::string> imageNames;
    if (imageArchive)
    {
        for (const genome::PakEntry &entry : imageArchive->entries())
        {
            if (entry.deleted || entry.path.size() < 6 ||
                entry.path.compare(entry.path.size() - 5, 5, ".ximg") != 0)
                continue;
            const std::size_t slash = entry.path.find_last_of('/');
            const std::string leaf = slash == std::string::npos ? entry.path : entry.path.substr(slash + 1);
            imageNames.insert(leaf.substr(0, leaf.size() - 5));
        }
    }
    const auto exists = [&](const std::string &name) { return imageNames.count(name) != 0; };

    // Water has no diffuse slot at all - its look comes from a dedicated
    // shader - so without a stand-in every river renders as white.
    auto water = std::make_unique<genome::Image>();
    water->width = 1;
    water->height = 1;
    water->faceCount = 1;
    water->format = genome::ImageFormat::A8R8G8B8;
    water->data = {150, 105, 45, 255}; // BGRA
    water->levels.push_back({1, 1, 0, 4});
    water->faceStride = 4;
    const genome::Image *waterImage = water.get();
    images.push_back(std::move(water));

    std::map<std::string, const genome::Image *> cache;
    // And a second cache on the file the material resolves to. Without it every
    // material sharing a diffuse map loads its own copy of the same pixels and
    // gets its own image, its own allocation and its own descriptor set: 913
    // materials in the shipping data resolve to 634 distinct files, one of them
    // named twenty-five times.
    std::map<std::string, const genome::Image *> imageOfFile;
    // What the materials in front of the camera actually offer. 633 across the
    // archive is a number about the archive; this one is about the scene.
    std::size_t materialsSeen = 0, materialsWithNormal = 0, normalsMissing = 0;
    std::size_t normalsSwizzled = 0, normalsPlain = 0, normalsOdd = 0;
    // Per material name, the normal map it resolved to. Kept beside the diffuse
    // cache rather than inside it because a material can name one without the
    // other.
    std::map<std::string, const genome::Image *> normalOf;
    // Which materials throw away transparent pixels, remembered per name so
    // the answer survives the texture cache.
    std::map<std::string, bool> masked;

    resolveTextures = [&, waterImage](std::vector<render::MeshInstances> &list) {
    const auto textureStart = now();
    struct TimeTextures
    {
        double &into;
        std::chrono::steady_clock::time_point start;
        ~TimeTextures()
        {
            into += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        }
    } timeTexturesGuard{timeTextures, textureStart};
    for (render::MeshInstances &batch : list)
    {
        if (!batch.mesh)
            continue;
        // Assigned rather than appended: this pass runs once per arriving
        // sector, and running it twice over one list would double both vectors
        // and misalign every element index against the mesh.
        batch.textures.clear();
        batch.normals.clear();
        batch.alphaTested.clear();
        for (const genome::MeshElement &element : batch.mesh->elements)
        {
            const genome::Image *loaded = nullptr;
            const auto cached = cache.find(element.materialName);
            if (cached != cache.end())
                loaded = cached->second;
            else if (imageArchive && element.materialName.size() > 4 &&
                     (element.materialName.compare(element.materialName.size() - 4, 4, ".dds") == 0 ||
                      element.materialName.compare(element.materialName.size() - 4, 4, ".tga") == 0))
            {
                // Grass and trees name their textures outright, in the form
                // they were authored under rather than through a material.
                std::string base = element.materialName.substr(0, element.materialName.size() - 4);
                for (char &c : base)
                    c = char(std::tolower(static_cast<unsigned char>(c)));

                std::string ignored;
                const auto already = imageOfFile.find(base);
                if (already != imageOfFile.end())
                    loaded = already->second;
                else
                {
                    auto image = std::make_unique<genome::Image>();
                    if (exists(base) && genome::loadImage(imageArchive->read(base + ".ximg", &ignored), *image,
                                                          &ignored))
                    {
                        loaded = image.get();
                        images.push_back(std::move(image));
                    }
                    imageOfFile.emplace(base, loaded);
                }
                cache.emplace(element.materialName, loaded);
                // Our own foliage - grass patches, leaves and fronds - is
                // quads whose texture is mostly empty.
                masked.emplace(element.materialName, true);
            }
            else if (materials && imageArchive && !element.materialName.empty())
            {
                genome::Material material;
                std::string ignored;
                if (genome::loadMaterial(materials->read(element.materialName, &ignored), material, &ignored))
                {
                    // The game says which surfaces mask; taking every alpha
                    // channel at face value punches holes through stone.
                    masked.emplace(element.materialName, material.blendMode == genome::BlendMode::Masked);
                    ++materialsSeen;
                    if (const genome::Sampler *bump = material.texture(genome::Slot::Normal))
                    {
                        const genome::TextureResolution resolvedBump = genome::resolveTexture(*bump, 0, exists);
                        if (resolvedBump.fileName.empty())
                            ++normalsMissing;
                        else
                        {
                            ++materialsWithNormal;
                            // Through the same by-file cache as the diffuse, so
                            // a normal map named by twenty materials is loaded
                            // once and uploaded once.
                            const auto already = imageOfFile.find(resolvedBump.fileName);
                            if (already != imageOfFile.end())
                                normalOf.emplace(element.materialName, already->second);
                            else
                            {
                                auto image = std::make_unique<genome::Image>();
                                const genome::Image *loadedBump = nullptr;
                                if (genome::loadImage(imageArchive->read(resolvedBump.fileName, &ignored), *image,
                                                      &ignored))
                                {
                                    loadedBump = image.get();
                                    images.push_back(std::move(image));
                                }
                                imageOfFile.emplace(resolvedBump.fileName, loadedBump);
                                normalOf.emplace(element.materialName, loadedBump);

                                // How it is encoded. Plain tangent-space RGB
                                // varies in red, green and blue; the swizzled
                                // layout of the era pins red and blue and puts
                                // X in alpha. Four samples of nature texture is
                                // not a rule, so every one is classified.
                                if (loadedBump)
                                {
                                    std::vector<std::uint8_t> decoded;
                                    if (genome::decodeLevel(*loadedBump, 0, 0, decoded, &ignored))
                                    {
                                        std::uint8_t low[4] = {255, 255, 255, 255};
                                        std::uint8_t high[4] = {0, 0, 0, 0};
                                        const std::size_t texels = decoded.size() / 4;
                                        for (std::size_t at = 0; at < texels; ++at)
                                            for (int channel = 0; channel < 4; ++channel)
                                            {
                                                const std::uint8_t value = decoded[at * 4 + channel];
                                                low[channel] = std::min(low[channel], value);
                                                high[channel] = std::max(high[channel], value);
                                            }
                                        const bool redFlat = high[0] - low[0] < 8;
                                        const bool blueFlat = high[2] - low[2] < 8;
                                        const bool alphaVaries = high[3] - low[3] > 32;
                                        if (redFlat && blueFlat && alphaVaries)
                                            ++normalsSwizzled;
                                        else if (!redFlat && !blueFlat)
                                            ++normalsPlain;
                                        else
                                        {
                                            ++normalsOdd;
                                            if (normalsOdd <= 3)
                                                std::printf("odd normal map %s: r %u..%u g %u..%u b %u..%u "
                                                            "a %u..%u\n",
                                                            resolvedBump.fileName.c_str(), low[0], high[0], low[1],
                                                            high[1], low[2], high[2], low[3], high[3]);
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if (material.kind == genome::ShaderKind::Water)
                        loaded = waterImage;
                    else if (const genome::Sampler *sampler = material.texture(genome::Slot::Diffuse))
                    {
                        const genome::TextureResolution resolved = genome::resolveTexture(*sampler, 0, exists);
                        if (!resolved.fileName.empty())
                        {
                            const auto already = imageOfFile.find(resolved.fileName);
                            if (already != imageOfFile.end())
                                loaded = already->second;
                            else
                            {
                                auto image = std::make_unique<genome::Image>();
                                if (genome::loadImage(imageArchive->read(resolved.fileName, &ignored), *image,
                                                      &ignored))
                                {
                                    loaded = image.get();
                                    images.push_back(std::move(image));
                                }
                                imageOfFile.emplace(resolved.fileName, loaded);
                            }
                        }
                    }
                }
                cache.emplace(element.materialName, loaded);
            }
            batch.textures.push_back(loaded);
            const auto bumped = normalOf.find(element.materialName);
            batch.normals.push_back(bumped != normalOf.end() ? bumped->second : nullptr);
            const auto isMasked = masked.find(element.materialName);
            batch.alphaTested.push_back(isMasked != masked.end() && isMasked->second ? 1 : 0);
        }
    }
    };

    everyBatch(resolveTextures);

    std::size_t untextured = 0;
    everyBatch([&](std::vector<render::MeshInstances> &list) {
        for (const render::MeshInstances &batch : list)
            for (const genome::Image *texture : batch.textures)
                untextured += texture == nullptr ? 1 : 0;
    });

    std::printf("%zu distinct textures, %zu mesh elements left untextured\n", images.size(), untextured);
    std::printf("%zu materials seen, %zu name a normal map that exists, %zu name one that does not\n",
                materialsSeen, materialsWithNormal, normalsMissing);
    std::printf("of those normal maps %zu are swizzled (x in alpha), %zu plain rgb, %zu neither\n",
                normalsSwizzled, normalsPlain, normalsOdd);
    if (untextured != 0)
    {
        // Name a few so the gap is diagnosable rather than just white.
        std::size_t named = 0;
        everyBatch([&](std::vector<render::MeshInstances> &list) {
            for (const render::MeshInstances &batch : list)
            {
                for (std::size_t element = 0; element < batch.textures.size() && named < 6; ++element)
                {
                    if (batch.textures[element] != nullptr)
                        continue;
                    const std::string &material = batch.mesh->elements[element].materialName;
                    std::printf("    no texture for %s\n",
                                material.empty() ? "(no material named)" : material.c_str());
                    ++named;
                }
            }
        });
    }

    render::Window window("Genome runtime - world", windowWidth, windowHeight);
    render::Device device;
    if (!device.create(window, &error, validation, uncapped))
    {
        std::cerr << "vulkan: " << error << "\n";
        return 1;
    }

    // Third detail level: a billboard. The trees we grew are drawn once each
    // into an atlas of our own, and past the far distance an instance becomes a
    // single quad sampling its own cell. The game shipped billboards but left
    // the field naming each tree's cell unset in 78 of its 98 definitions, so
    // baking our own is both easier and more correct.
    render::TreeAtlas treeAtlas;
    genome::Image treeAtlasImage;
    std::vector<std::unique_ptr<genome::Mesh>> billboardMeshes;
    std::map<std::string, genome::Mesh *> cardOf;
    std::size_t billboardsMissed = 0;
    // And what that costs: the trees that will never draw a billboard, and how
    // many kinds the atlas was baked for against how many turned up in the end.
    std::size_t billboardInstancesMissed = 0;
    std::size_t kindsAtBake = 0, kindsSubstituted = 0;

    // A sector's trees get their third detail level: the same instances as the
    // full-detail form, drawn as a quad sampling that kind's cell. A kind first
    // seen after the atlas was baked has no cell, so its trees keep the thinned
    // mesh all the way out - which is worse to look at and correct to draw.
    const auto attachBillboards = [&](SectorContent &content) {
        std::size_t added = 0;
        const std::size_t before = content.batches.size();
        for (const auto &[kind, slot] : content.treeSlots)
        {
            const genome::Mesh *card = nullptr;
            const auto exact = cardOf.find(kind);
            if (exact != cardOf.end())
                card = exact->second;
            else if (!kind.empty())
            {
                // A kind is a definition plus a variant digit, and the variants
                // are one tree grown from different seeds. At four hundred
                // metres, which is the only distance a billboard is seen from,
                // a sibling's card is that tree.
                const std::string stem = kind.substr(0, kind.size() - 1);
                for (std::uint32_t variant = 0; variant < c_TreeVariants && !card; ++variant)
                {
                    const auto sibling = cardOf.find(stem + char('0' + variant));
                    if (sibling != cardOf.end())
                        card = sibling->second;
                }
                kindsSubstituted += card ? 1 : 0;
            }

            if (!card || slot >= before)
            {
                if (!card)
                {
                    ++billboardsMissed;
                    if (slot < before)
                        billboardInstancesMissed += content.batches[slot].transforms.size();
                }
                continue;
            }

            render::MeshInstances billboard;
            billboard.mesh = card;
            billboard.textures.push_back(&treeAtlasImage);
            billboard.alphaTested.push_back(1);
            billboard.faceCamera = true;
            billboard.occludes = false;
            billboard.lodNear = treeBillboardDistance;
            billboard.transforms = content.batches[slot].transforms;
            billboard.bounds = content.batches[slot].bounds;
            added += billboard.transforms.size();
            content.batches.push_back(std::move(billboard));

            // The thinned tree now ends where the billboard begins.
            if (slot + 1 < before)
                content.batches[slot + 1].lodFar = treeBillboardDistance;
        }
        return added;
    };

    if (!treeKinds.empty())
    {
        std::vector<render::MeshInstances> bakeBatches;
        std::vector<std::size_t> bakeOrder;
        for (const std::string &kind : treeKinds)
        {
            render::MeshInstances one;
            one.mesh = treeMeshOf[kind][0];
            one.transforms.push_back(c_Identity);
            // The mesh sits at the origin, so its own bounds fit the camera.
            std::array<float, 6> box{1e9f, 1e9f, 1e9f, -1e9f, -1e9f, -1e9f};
            for (const genome::MeshElement &element : one.mesh->elements)
                for (const std::array<float, 3> &position : element.positions)
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        box[axis] = std::min(box[axis], position[axis]);
                        box[axis + 3] = std::max(box[axis + 3], position[axis]);
                    }
            one.bounds.push_back(box);
            bakeOrder.push_back(bakeBatches.size());
            bakeBatches.push_back(std::move(one));
        }
        resolveTextures(bakeBatches);

        render::WorldRenderer baker;
        if (baker.create(device, bakeBatches, &error) &&
            render::bakeTreeAtlas(device, baker, bakeOrder, 256, treeAtlas, &error) &&
            render::readTreeAtlas(device, treeAtlas, treeAtlasImage, &error))
        {
            std::printf("baked %zu tree billboards into a %ux%u atlas\n", treeAtlas.cells.size(), treeAtlas.size,
                        treeAtlas.size);

            // One quad per kind, with that kind's cell baked into its texture
            // coordinates. Every sector that planted this kind of tree points
            // at the same quad, so the renderer keeps them in one batch.
            for (std::size_t index = 0; index < treeKinds.size() && index < treeAtlas.cells.size(); ++index)
            {
                const std::array<float, 4> &cell = treeAtlas.cells[index];
                auto mesh = std::make_unique<genome::Mesh>();
                genome::MeshElement card;
                card.positions = {{-0.5f, 0.0f, 0.0f}, {0.5f, 0.0f, 0.0f}, {0.5f, 1.0f, 0.0f}, {-0.5f, 1.0f, 0.0f}};
                card.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
                card.indices = {0, 1, 2, 0, 2, 3};
                card.materialName = "tree billboard";
                card.texCoords = {{cell[0], cell[3]}, {cell[2], cell[3]}, {cell[2], cell[1]}, {cell[0], cell[1]}};
                mesh->elements.push_back(std::move(card));
                cardOf.emplace(treeKinds[index], mesh.get());
                billboardMeshes.push_back(std::move(mesh));
                kindsAtBake = cardOf.size();
            }

            std::size_t billboardInstances = 0;
            for (SectorContent &content : residentSectors)
                billboardInstances += attachBillboards(content);
            std::printf("billboards cover %zu tree instances beyond %.0f units\n", billboardInstances,
                        treeBillboardDistance);
        }
        else
            std::printf("warning: no tree billboards: %s\n", error.c_str());
        baker.destroy(device);
    }

    std::printf("loading: %.1fs reading sectors, %.1fs meshes, %.1fs lightmaps, %.1fs growing trees\n",
                timeReadingSectors, timeMeshes, timeLightmaps, timeTrees);
    std::printf("%zu instances lit by a baked lightmap, %zu without one, %zu colours, %zu direction floats\n",
                lightmapsFound, lightmapsMissing, lightmapColours.size(), lightmapIncident.size());
    std::printf("patches cover %.1f%% of the atlas in %zu of %u tiles (%.0f%% of each used); widest %zu, "
                "tallest %zu\n",
                100.0 * double(patchAtlas.texels) / double(PatchAtlas::c_Size) / double(PatchAtlas::c_Size),
                patchAtlas.tilesTaken, PatchAtlas::c_Grid * PatchAtlas::c_Grid,
                100.0 * double(patchAtlas.texels) /
                    double(std::max<std::size_t>(patchAtlas.tilesTaken, 1) * PatchAtlas::c_Tile * PatchAtlas::c_Tile),
                patchAtlas.widest, patchAtlas.tallest);
    std::printf("%zu baked patches packed, %zu refused, %zu vertices given one; sizes match the charts %zu to %zu\n",
                patchAtlas.packed, patchAtlas.refused, patchedVertices, sizeMatches, sizeMismatches);
    std::printf("baked patches %s the daylight where they cover\n",
                lightmapReplaces > 0.5f ? "replace" : "add to");

    // Everything the lighting needs has to be handed over BEFORE the renderer is
    // created: create() is where the buffers are filled and the descriptors
    // written. Handing it over afterwards leaves the shader reading a one-entry
    // placeholder, and out-of-range reads on a storage buffer come back as
    // zeros - so the lighting was not wrong, it was absent, and every attempt to
    // change it changed nothing at all.
    render::WorldRenderer renderer;
    // Everything that is not a sector's: the landscape's own lights and the
    // vegetation layers'. The rest are added back whenever the resident set
    // changes, which is the only way a sector's torches arrive with it.
    const std::vector<genome::PointLight> baseLights = worldLights;
    for (const SectorContent &content : residentSectors)
        worldLights.insert(worldLights.end(), content.lights.begin(), content.lights.end());
    renderer.setLights(worldLights);
    renderer.setOcclusionThreshold(occlusionPixels);
    renderer.setNormalStrength(normalStrength);
    if (cullThreads > 0)
        renderer.setCullThreads(unsigned(cullThreads));
    // Jitter sets the grain to one itself, so an explicit grain is applied
    // after it rather than before.
    if (cullJitter > 0)
    {
        renderer.setCullJitter(unsigned(cullJitter));
    }
    if (cullGrain > 0)
        renderer.setCullGrain(unsigned(cullGrain));
    if (cullJitter > 0)
    {
        std::printf("culling with jitter up to %d spins a batch, %u batches at a time\n", cullJitter,
                    renderer.cullGrain());
    }
    renderer.setLightmaps(std::move(lightmapColours));
    renderer.setLightmapDirections(std::move(lightmapIncident));
    lightmapCoords.resize(std::max<std::size_t>(lightmapCoords.size(), 2), -1.0f);
    renderer.setLightmapCoords(std::move(lightmapCoords));
    renderer.setLightmapAtlas(&patchAtlas.image);

    const auto rendererStart = now();

    // What the arenas are built to hold. Measured from what is loaded, with
    // room to spare when sectors will be coming and going: an arrival has to
    // land somewhere before the departure that makes room for it.
    render::WorldRenderer::Budget budget;
    {
        std::size_t budgetVertices = 0, budgetIndices = 0, budgetInstances = 0, budgetLit = 0;
        std::set<const genome::Mesh *> counted;
        everyBatch([&](std::vector<render::MeshInstances> &list) {
            for (const render::MeshInstances &batch : list)
            {
                budgetInstances += batch.transforms.size();
                if (!batch.mesh || !counted.insert(batch.mesh).second)
                    continue;
                for (const genome::MeshElement &element : batch.mesh->elements)
                {
                    if (element.positions.empty() || element.indices.empty())
                        continue;
                    budgetVertices += element.positions.size();
                    budgetIndices += element.indices.size();
                }
            }
        });
        for (const SectorContent &content : residentSectors)
            budgetLit += content.lighting.colours.size();

        const double room = budgetRoom > 0.0 ? budgetRoom : (streaming ? 2.0 : 1.02);
        if (budgetRoom > 0.0)
            std::printf("squeezed: the arenas get %.2f times what the first resident set needs\n", budgetRoom);
        budget.vertices = std::size_t(double(budgetVertices) * room) + 4096;
        budget.indices = std::size_t(double(budgetIndices) * room) + 4096;
        budget.lightVertices = std::size_t(double(budgetLit) * room) + 4096;
        budget.instances = std::size_t(double(budgetInstances) * room) + 4096;
    }

    if (!renderer.create(device, budget, &error))
    {
        std::cerr << "renderer: " << error << "\n";
        return 1;
    }

    // Sector zero is everything that is not a sector - the landscape, and a
    // single tree when one was asked for - and it never leaves. The rest go in
    // one at a time, by exactly the call an arrival will use later.
    if (!batches.empty() && !renderer.addSector(device, 0, batches, {}, &error))
    {
        std::cerr << "renderer: " << error << "\n";
        return 1;
    }
    for (SectorContent &content : residentSectors)
    {
        if (!renderer.addSector(device, content.id, content.batches, content.lighting, &error))
        {
            std::cerr << "renderer: " << error << "\n";
            return 1;
        }
    }
    renderer.reportArenas();
    if (const char *dump = std::getenv("G3_DUMP_ATLAS"))
    {
        // The packed patches, so the packing can be looked at rather than
        // trusted.
        if (std::FILE *out = std::fopen(dump, "wb"))
        {
            std::fprintf(out, "P6\n%u %u\n255\n", patchAtlas.image.width, patchAtlas.image.height);
            for (std::size_t at = 0; at < patchAtlas.image.data.size(); at += 4)
            {
                std::fputc(patchAtlas.image.data[at + 2], out);
                std::fputc(patchAtlas.image.data[at + 1], out);
                std::fputc(patchAtlas.image.data[at + 0], out);
            }
            std::fclose(out);
            std::printf("wrote the patch atlas to %s\n", dump);
        }
    }

    timeRenderer = since(rendererStart);
    std::printf("loading: %.1fs textures, %.1fs uploading to the card\n", timeTextures, timeRenderer);

    // Before the loop: setting this up records and submits a command buffer of
    // its own to calibrate the card's clock against ours, which cannot happen
    // while a frame is being recorded.
    renderer.startProfiling(device);

    const std::array<float, 3> &min = renderer.boundsMin();
    const std::array<float, 3> &max = renderer.boundsMax();
    const std::array<float, 3> centre{(min[0] + max[0]) * 0.5f, (min[1] + max[1]) * 0.5f, (min[2] + max[2]) * 0.5f};
    const float span = std::max(max[0] - min[0], max[2] - min[2]);
    std::printf("world spans %.0f x %.0f x %.0f units (%.1f x %.1f km), %zu draws\n", max[0] - min[0],
                max[1] - min[1], max[2] - min[2], (max[0] - min[0]) / 100000.0f,
                (max[2] - min[2]) / 100000.0f, renderer.drawCount());

    // Spectator camera: look with the right mouse button held, move with WASD,
    // rise and fall with E and Q. Shift accelerates, and the speed scales with
    // the world so the same controls work on a hut and on the whole map.
    std::array<float, 3> eye{centre[0], centre[1] + span * 0.25f, centre[2] + span * 0.6f};
    // Start looking at what was loaded, whatever its size, rather than at a
    // fixed heading that only suits one scale.
    const std::array<float, 3> toCentre{centre[0] - eye[0], centre[1] - eye[1], centre[2] - eye[2]};
    float yaw = std::atan2(toCentre[0], toCentre[2]);
    float pitch = std::atan2(toCentre[1], std::sqrt(toCentre[0] * toCentre[0] + toCentre[2] * toCentre[2]));

    // An explicit viewpoint makes a picture repeatable, which the automatic
    // framing cannot be once the loaded extent changes.
    if (cameraArgument != 0)
    {
        eye = {float(std::atof(argv[cameraArgument])), float(std::atof(argv[cameraArgument + 1])),
               float(std::atof(argv[cameraArgument + 2]))};
        yaw = float(std::atof(argv[cameraArgument + 3])) * (cameraInRadians ? 1.0f : 3.14159265f / 180.0f);
        pitch = float(std::atof(argv[cameraArgument + 4])) * (cameraInRadians ? 1.0f : 3.14159265f / 180.0f);
        std::printf("camera at %.0f %.0f %.0f looking %s %s degrees\n", eye[0], eye[1], eye[2],
                    argv[cameraArgument + 3], argv[cameraArgument + 4]);
    }
    bool looking = false;
    bool occlusion = startWithOcclusion;
    POINT lastCursor{};

    std::vector<float> frameTimes, cullTimes;
    // What each frame did, so the worst one can be asked about rather than
    // guessed at. Two guesses have already been measured and been wrong.
    std::vector<std::uint8_t> frameWork;
    std::uint8_t workThisFrame = 0;
    // Where a frame's time went, in six parts that add up to the whole of it.
    struct FramePhases
    {
        float input = 0.0f, streaming = 0.0f, begin = 0.0f, cull = 0.0f, record = 0.0f, present = 0.0f;
        // The three things beginFrame can block in, which together make up begin.
        float retire = 0.0f, fence = 0.0f, acquire = 0.0f;
        float total() const { return input + streaming + begin + cull + record + present; }
    };
    FramePhases phases;
    std::vector<FramePhases> framePhases;
    const auto millisSince = [](std::chrono::steady_clock::time_point from) {
        return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - from).count();
    };
    if (benchFrames > 0)
    {
        frameTimes.reserve(std::size_t(benchFrames));
        cullTimes.reserve(std::size_t(benchFrames));
    }

    auto previous = std::chrono::steady_clock::now();
    std::size_t frames = 0;
    std::vector<std::string> arriving;
    // Atlas tiles a departed sector gave back, held until no submitted frame
    // can still be sampling them. The next arrival would otherwise take one and
    // paint over it mid-frame.
    std::vector<std::pair<std::uint64_t, std::vector<std::uint32_t>>> freedTiles;
    std::size_t sectorsArrived = 0, sectorsDeparted = 0, sectorsUnwanted = 0, sectorsRefused = 0;
    // Kept apart from the arrivals: a refusal costs real milliseconds, and
    // adding them to a total divided by the number of successes made arrivals
    // look slower when none of them was.
    float refusedCost = 0.0f;
    // Set whenever the resident set changes; the list is rebuilt once, before
    // the cull that reads it, rather than on each arrival and each departure.
    bool lightsStale = false;
    std::size_t lightsAtStart = worldLights.size(), lightsNow = worldLights.size(), lightsMost = worldLights.size();
    float worstArrival = 0.0f, totalArrival = 0.0f;

    // Everything a sector needs read and parsed, on the loader's thread. Its
    // timing accumulators are written here too, and read only after the loader
    // has stopped.
    SectorLoader loader;
    loader.start(
        [&](const genome::PakEntry &entry, std::uint32_t id, LoadedSector &out) {
            out.content.name = entry.path;
            out.content.id = id;
            patchAtlas.beginSector();
            if (!loadSector(entry, out.content))
                return;
            out.content.tiles = std::move(patchAtlas.sectorTiles);
            resolveTextures(out.content.batches);
            attachBillboards(out.content);

            // Gathered here rather than on the frame, because a tile is a
            // window into a 4096-wide image and the image is ours.
            const std::size_t tileBytes = std::size_t(PatchAtlas::c_Tile) * PatchAtlas::c_Tile * 4;
            out.tilePixels.resize(tileBytes * out.content.tiles.size());
            for (std::size_t which = 0; which < out.content.tiles.size(); ++which)
            {
                const std::uint32_t index = out.content.tiles[which];
                const std::uint32_t tx = (index % PatchAtlas::c_Grid) * PatchAtlas::c_Tile;
                const std::uint32_t ty = (index / PatchAtlas::c_Grid) * PatchAtlas::c_Tile;
                std::uint8_t *into = out.tilePixels.data() + which * tileBytes;
                for (std::uint32_t row = 0; row < PatchAtlas::c_Tile; ++row)
                    std::memcpy(into + std::size_t(row) * PatchAtlas::c_Tile * 4,
                                &patchAtlas.image.data[(std::size_t(ty + row) * PatchAtlas::c_Size + tx) * 4],
                                std::size_t(PatchAtlas::c_Tile) * 4);
            }
            out.ok = true;
        },
        [&](const std::vector<std::uint32_t> &tiles) { patchAtlas.freeTiles(tiles); });
    // What an arrival is made of, split by which thread pays for it. The
    // loading accumulators belong to the loader's thread now, so they are read
    // once, after it has stopped, as deltas from where they stood at startup.
    const double startRead = timeReadingSectors, startMeshes = timeMeshes;
    const double startLight = timeLightmaps, startTrees = timeTrees, startTextures = timeTextures;
    double arrivalUpload = 0.0, arrivalPatches = 0.0;

    while (window.pump())
    {
        const auto now = std::chrono::steady_clock::now();
        // A benched flight uses a fixed step rather than the real frame time, so
        // that the same command flies the same path every run and two
        // measurements can be compared at all.
        const float delta = benchFrames > 0 ? 1.0f / 60.0f
                                            : std::min(std::chrono::duration<float>(now - previous).count(), 0.1f);
        previous = now;

        const bool wantsLook = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        POINT cursor{};
        GetCursorPos(&cursor);
        if (wantsLook && looking)
        {
            yaw -= float(cursor.x - lastCursor.x) * 0.005f;
            pitch = std::clamp(pitch - float(cursor.y - lastCursor.y) * 0.005f, -1.5f, 1.5f);
        }
        looking = wantsLook;
        lastCursor = cursor;

        const std::array<float, 3> forward{std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                                           std::cos(pitch) * std::cos(yaw)};
        const std::array<float, 3> right{std::sin(yaw - 1.5708f), 0.0f, std::cos(yaw - 1.5708f)};

        float speed = span * 0.12f * delta;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
            speed *= 6.0f;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000)
            speed *= 0.15f;

        const auto move = [&](const std::array<float, 3> &direction, float scale) {
            for (int axis = 0; axis < 3; ++axis)
                eye[axis] += direction[axis] * scale;
        };
        if (window.keyDown('W'))
            move(forward, speed);
        if (window.keyDown('S'))
            move(forward, -speed);
        if (window.keyDown('D'))
            move(right, speed);
        if (window.keyDown('A'))
            move(right, -speed);
        if (window.keyDown('E'))
            eye[1] += speed;
        if (window.keyDown('Q'))
            eye[1] -= speed;
        if (window.keyPressed('O'))
            occlusion = !occlusion;
        if (flySpeed != 0.0f)
            move(forward, flySpeed * delta);

        phases.input = millisSince(now);
        const auto streamingStart = std::chrono::steady_clock::now();

        // Sectors arrive and leave as the camera moves. The rectangle changing
        // is the cheap test; the distance is what stops a camera sitting on a
        // cell boundary from thrashing one sector in and out, and both are the
        // game's own conditions.
        if (streaming && world)
        {
            for (std::size_t index = 0; index < freedTiles.size();)
            {
                if (device.frameCounter() < freedTiles[index].first + render::Device::c_FramesInFlight + 1)
                {
                    ++index;
                    continue;
                }
                loader.giveBackTiles(std::move(freedTiles[index].second));
                freedTiles.erase(freedTiles.begin() + std::ptrdiff_t(index));
            }

            const genome::ResidentCells want = genome::residentCells(eye, c_ResidencyFar);
            const float dx = eye[0] - residencyEye[0], dz = eye[2] - residencyEye[2];
            const float gate = std::max(1300.0f, c_ResidencyFar * 0.2f);
            if (!(want == heldCells) && dx * dx + dz * dz >= gate * gate)
            {
                heldCells = want;
                residencyEye = eye;

                // Departures first: they are what makes room for the arrivals.
                for (std::size_t index = 0; index < residentSectors.size();)
                {
                    const auto box = boundsOf.find(sectorKey(residentSectors[index].name));
                    if (box != boundsOf.end() && genome::overlaps(box->second, heldCells))
                    {
                        ++index;
                        continue;
                    }
                    workThisFrame |= 2;
                    lightsStale = true;
                    renderer.dropSector(device, residentSectors[index].id);
                    freedTiles.emplace_back(device.frameCounter(), std::move(residentSectors[index].tiles));
                    residentSectors.erase(residentSectors.begin() + std::ptrdiff_t(index));
                    ++sectorsDeparted;
                }

                arriving.clear();
                for (const genome::PakEntry &entry : world->entries())
                {
                    if (entry.deleted || entry.path.find(sectorFilter) == std::string::npos)
                        continue;
                    const auto box = boundsOf.find(sectorKey(entry.path));
                    if (box == boundsOf.end() || !genome::overlaps(box->second, heldCells))
                        continue;
                    bool alreadyHere = false;
                    for (const SectorContent &content : residentSectors)
                        alreadyHere = alreadyHere || content.name == entry.path;
                    if (!alreadyHere)
                        arriving.push_back(entry.path);
                }
            }

            // One at a time, and not on this thread. The loader owns the
            // archives and the caches, so it takes a path and gives back a
            // finished sector; the frame does the Vulkan half only.
            if (!arriving.empty() && !loader.busy())
            {
                const std::string path = arriving.back();
                arriving.pop_back();
                for (const genome::PakEntry &one : world->entries())
                    if (!one.deleted && one.path == path)
                    {
                        loader.request(one, nextSectorId++);
                        break;
                    }
            }

            LoadedSector loaded;
            if (loader.take(loaded) && loaded.ok)
            {
                // It may no longer be wanted: the camera can turn round while a
                // sector is being read. Giving the tiles back is all the
                // undoing needed, since nothing else reached the card yet.
                const auto box = boundsOf.find(sectorKey(loaded.content.name));
                const bool wanted = box != boundsOf.end() && genome::overlaps(box->second, heldCells);
                if (!wanted)
                {
                    loader.giveBackTiles(std::move(loaded.content.tiles));
                    ++sectorsUnwanted;
                }
                else
                {
                    const auto arrivalStart = std::chrono::steady_clock::now();
                    SectorContent &content = loaded.content;
                    if (!renderer.addSector(device, content.id, content.batches, content.lighting, &error))
                    {
                        // It put itself back, so the tiles have to go back too -
                        // otherwise they are held by a sector that is not there.
                        std::printf("warning: %s did not fit: %s\n", content.name.c_str(), error.c_str());
                        loader.giveBackTiles(std::move(content.tiles));
                        ++sectorsRefused;
                        refusedCost += std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() -
                                                                               arrivalStart).count();
                    }
                    else
                    {
                        arrivalUpload += since(arrivalStart);
                        const auto patchStart = std::chrono::steady_clock::now();

                        // The tiles it brought, in one submit. The pixels came
                        // with it: the atlas belongs to the loader's thread.
                        const std::size_t tileBytes = std::size_t(PatchAtlas::c_Tile) * PatchAtlas::c_Tile * 4;
                        std::vector<render::TextureRegion> regions;
                        regions.reserve(content.tiles.size());
                        for (std::size_t which = 0; which < content.tiles.size(); ++which)
                        {
                            const std::uint32_t index = content.tiles[which];
                            const std::uint32_t tx = (index % PatchAtlas::c_Grid) * PatchAtlas::c_Tile;
                            const std::uint32_t ty = (index / PatchAtlas::c_Grid) * PatchAtlas::c_Tile;
                            regions.push_back({tx, ty, PatchAtlas::c_Tile, PatchAtlas::c_Tile,
                                               loaded.tilePixels.data() + which * tileBytes});
                        }
                        renderer.updatePatchAtlas(device, regions, &error);
                        arrivalPatches += since(patchStart);
                        workThisFrame |= 1;
                        lightsStale = true;
                        residentSectors.push_back(std::move(content));
                        ++sectorsArrived;

                        // Timed on this branch only. A refusal's milliseconds
                        // used to land in a total divided by the number of
                        // successes, which made arrivals look slower when none
                        // of them was.
                        const float cost = std::chrono::duration<float, std::milli>(
                            std::chrono::steady_clock::now() - arrivalStart).count();
                        worstArrival = std::max(worstArrival, cost);
                        totalArrival += cost;
                    }
                }
            }
        }

        if (lightsStale)
        {
            lightsStale = false;
            worldLights = baseLights;
            for (const SectorContent &content : residentSectors)
                worldLights.insert(worldLights.end(), content.lights.begin(), content.lights.end());
            renderer.setLights(worldLights);
            lightsNow = worldLights.size();
            lightsMost = std::max(lightsMost, lightsNow);
        }

        const std::array<float, 3> target{eye[0] + forward[0], eye[1] + forward[1], eye[2] + forward[2]};

        const VkExtent2D extent = device.extent();
        const float aspect = float(extent.width) / float(extent.height);
        const std::array<float, 16> viewProjection =
            multiply(perspective(1.0f, aspect, 50.0f, span * 4.0f), lookAt(eye, target));

        phases.streaming = millisSince(streamingStart);
        const auto beginStart = std::chrono::steady_clock::now();
        if (!device.beginFrame())
            continue;

        VkCommandBuffer command = device.commandBuffer();

        VkImageMemoryBarrier toAttachment{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toAttachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toAttachment.image = device.currentColorImage();
        toAttachment.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toAttachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toAttachment);

        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = device.currentColorView();
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0.53f, 0.66f, 0.79f, 1.0f}}; // daylight sky

        VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depthAttachment.imageView = device.depthView();
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.clearValue.depthStencil = {1.0f, 0};

        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, extent};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        rendering.pDepthAttachment = &depthAttachment;
        phases.begin = millisSince(beginStart);
        phases.retire = device.lastRetireMillis();
        phases.fence = device.lastFenceMillis();
        phases.acquire = device.lastAcquireMillis();
        // Everything from here to the end of the cull is counted as the cull's,
        // because beginning the pass and setting the viewport are the frame's
        // work too and used to sit in a gap between two phases.
        const auto cullStart = std::chrono::steady_clock::now();
        vkCmdBeginRendering(command, &rendering);

        VkViewport viewport{0.0f, 0.0f, float(extent.width), float(extent.height), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetViewport(command, 0, 1, &viewport);
        vkCmdSetScissor(command, 0, 1, &scissor);

        // A pixel and a half: below that an object is a speck, whatever it is.
        const float pixelsPerRadian = float(extent.height) * 0.5f / std::tan(0.5f);
        for (int pass = 0; pass < cullRepeat; ++pass)
            renderer.cull(device, viewProjection, eye, pixelsPerRadian, 1.5f, occlusion);
        phases.cull = millisSince(cullStart);
        const auto recordStart = std::chrono::steady_clock::now();
        if (benchFrames > 0)
            cullTimes.push_back(std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() -
                                                                        cullStart).count());

        static float reportAt = 0.0f;
        reportAt += delta;
        if (reportAt > 1.0f)
        {
            reportAt = 0.0f;
            std::printf("visible %zu of %zu (%zu too small, %zu occluded%s)\n", renderer.visibleInstances(),
                        renderer.instanceCount(), renderer.tooSmallInstances(), renderer.occludedInstances(),
                        occlusion ? "" : ", occlusion off");
        }
        renderer.draw(device, viewProjection, {0.45f, 0.75f, 0.35f, lightmapReplaces});

        vkCmdEndRendering(command);
        // Outside the render pass, which is where the profiler may write to its
        // query pool.
        renderer.collectProfiling(device);

        VkImageMemoryBarrier toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.image = device.currentColorImage();
        toPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toPresent.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &toPresent);

        phases.record = millisSince(recordStart);
        const auto presentStart = std::chrono::steady_clock::now();
        device.endFrame();
        phases.present = millisSince(presentStart);
        G3_FRAME_MARK;

        if (benchFrames > 0)
        {
            frameTimes.push_back(
                std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - now).count());
            frameWork.push_back(workThisFrame);
            framePhases.push_back(phases);
            workThisFrame = 0;
            phases = FramePhases{};
            if (int(frameTimes.size()) >= benchFrames)
            {
                // The first frames pay for pipeline warm-up and uploads, so they
                // are not what a steady frame costs.
                const std::size_t skip = std::min<std::size_t>(10, frameTimes.size() / 4);
                auto report = [skip](std::vector<float> &times, const char *label) {
                    std::vector<float> kept(times.begin() + skip, times.end());
                    std::sort(kept.begin(), kept.end());
                    float total = 0.0f;
                    for (float value : kept)
                        total += value;
                    std::printf("%-6s mean %6.2f ms   median %6.2f   95th %6.2f   worst %6.2f\n", label,
                                total / float(kept.size()), kept[kept.size() / 2],
                                kept[std::size_t(float(kept.size()) * 0.95f)], kept.back());
                };
                // The extent the swapchain really got, not the one asked for:
                // a window larger than the desktop is clamped, and a number
                // measured at a size that was never used is worse than none.
                std::printf("drawing %ux%u, presenting %s, culling on %u threads\n", device.extent().width,
                            device.extent().height, device.presentModeName(), renderer.cullThreads());
                report(frameTimes, "frame");
                report(cullTimes, "cull");
                {
                    const render::WorldRenderer::CullPhases &split = renderer.cullPhases();
                    std::printf("the last cull went: %.2f prologue, %.2f occluders, %.2f cells, %.2f instances, "
                                "%.2f lights\n",
                                split.prologue, split.occluders, split.cells, split.instances, split.lights);
                    const std::vector<float> busy = renderer.threadBusy();
                    if (busy.size() > 1)
                    {
                        float total = 0.0f;
                        for (float one : busy)
                            total += one;
                        std::printf("threads were busy %.2f ms at most, %.2f at least, %.2f on average; "
                                    "the sum is %.2f against %.2f of wall\n",
                                    busy.front(), busy.back(), total / float(busy.size()), total, split.instances);
                    }
                }

                // Which frame was the worst, and what it was doing.
                std::size_t worst = skip;
                for (std::size_t at = skip; at < frameTimes.size(); ++at)
                    if (frameTimes[at] > frameTimes[worst])
                        worst = at;
                // The median frame, which is what the frame actually costs -
                // the worst one says where a spike went, not where the time is.
                if (!framePhases.empty())
                {
                    std::vector<FramePhases> ordered(framePhases.begin() + std::ptrdiff_t(skip), framePhases.end());
                    std::sort(ordered.begin(), ordered.end(),
                              [](const FramePhases &a, const FramePhases &b) { return a.total() < b.total(); });
                    const auto pick = [&ordered](float FramePhases::*part) {
                        std::vector<float> values;
                        values.reserve(ordered.size());
                        for (const FramePhases &one : ordered)
                            values.push_back(one.*part);
                        std::sort(values.begin(), values.end());
                        return values[values.size() / 2];
                    };
                    // Each phase's own median, not one frame's: the parts come from
                    // different frames and need not add up to any of them.
                    std::printf("the median of each phase: %.2f input, %.2f streaming, %.2f waiting to begin, %.2f cull, "
                                "%.2f recording, %.2f presenting\n",
                                pick(&FramePhases::input), pick(&FramePhases::streaming), pick(&FramePhases::begin),
                                pick(&FramePhases::cull), pick(&FramePhases::record), pick(&FramePhases::present));
                    std::printf("and the median wait was: %.2f retiring, %.2f on our own fence, %.2f acquiring\n",
                                pick(&FramePhases::retire), pick(&FramePhases::fence), pick(&FramePhases::acquire));
                }
                if (worst < framePhases.size())
                {
                    const FramePhases &split = framePhases[worst];
                    std::printf("that frame went: %.1f input, %.1f streaming, %.1f waiting to begin, %.1f cull, "
                                "%.1f recording, %.1f presenting - %.1f of %.1f accounted\n",
                                split.input, split.streaming, split.begin, split.cull, split.record, split.present,
                                split.total(), frameTimes[worst]);
                    std::printf("and the waiting was: %.1f retiring transfers, %.1f on our own fence, "
                                "%.1f acquiring an image\n",
                                split.retire, split.fence, split.acquire);
                }
                const std::uint8_t did = worst < frameWork.size() ? frameWork[worst] : 0;
                std::printf("the worst frame was number %zu of %zu at %.2f ms, and it %s\n", worst,
                            frameTimes.size(), frameTimes[worst],
                            did == 3   ? "both took a sector in and dropped some"
                            : did == 1 ? "took a sector in"
                            : did == 2 ? "dropped sectors"
                                       : "neither took nor dropped anything");
                std::printf("%.2fM instances walked by the cull\n", double(renderer.testedInstances()) / 1e6);
                std::printf("%zu draws, %.2fM triangles submitted\n", renderer.submittedDraws(),
                            double(renderer.submittedTriangles()) / 1e6);
                std::printf("%zu of %zu instances drawn; %zu rejected whole, %zu outside, %zu wrong detail, "
                            "%zu too small, %zu occluded, %td unaccounted\n",
                            renderer.visibleInstances(), renderer.instanceCount(),
                            renderer.rejectedWholeInstances(), renderer.outsideViewInstances(),
                            renderer.wrongLodInstances(), renderer.tooSmallInstances(),
                            renderer.occludedInstances(), renderer.unaccountedInstances());
                if (streaming)
                {
                    std::printf("%zu sectors arrived and %zu left while flying; %zu resident now\n", sectorsArrived,
                                sectorsDeparted, renderer.sectorCount());
                    if (sectorsArrived != 0)
                        std::printf("arrivals cost %.0f ms at worst and %.0f ms on average, %.0f ms in all\n",
                                    worstArrival, totalArrival / float(sectorsArrived), totalArrival);
                    // Nothing is loading any more, so its accumulators can be
                    // read.
                    loader.stop();
                    if (sectorsArrived != 0)
                    {
                        std::printf("on the frame: %.0f ms uploading, %.0f ms patches, %.0f ms unaccounted\n",
                                    arrivalUpload * 1000.0, arrivalPatches * 1000.0,
                                    double(totalArrival) - (arrivalUpload + arrivalPatches) * 1000.0);
                        std::printf("on the loader: %.0f ms reading sectors, %.0f meshes, %.0f lightmaps, "
                                    "%.0f trees, %.0f textures\n",
                                    (timeReadingSectors - startRead) * 1000.0, (timeMeshes - startMeshes) * 1000.0,
                                    (timeLightmaps - startLight) * 1000.0, (timeTrees - startTrees) * 1000.0,
                                    (timeTextures - startTextures) * 1000.0);
                    }
                    if (sectorsUnwanted != 0)
                        std::printf("%zu sectors finished loading after the camera had left them\n",
                                    sectorsUnwanted);
                    if (sectorsRefused != 0)
                        std::printf("%zu sectors did not fit and were put back, costing %.0f ms in all\n",
                                    sectorsRefused, refusedCost);
                    std::printf("lights: %zu at the start, %zu now, %zu at the most\n", lightsAtStart, lightsNow,
                                lightsMost);
                    std::printf("%zu transfers still in flight, %zu drains paid to keep the bound\n",
                                device.transfersInFlight(), device.transferStalls());
                    if (renderer.staleArrivals() != 0)
                        std::printf("%zu arrivals landed on a frame that had already dropped a sector; "
                                    "%zu lookups would have named the wrong batch and %zu one past the end\n",
                                    renderer.staleArrivals(), renderer.staleLookups(),
                                    renderer.staleOutOfRange());
                    std::printf("%zu textures freed as their sectors left, %zu came back, %zu still held\n",
                                renderer.texturesFreed(), renderer.texturesRemade(), renderer.textureCount());
                    std::printf("the biggest retirement on one frame destroyed %zu textures\n",
                                renderer.worstRetireBurst());
                    if (device.swapchainRebuilds() != 0)
                        std::printf("the swapchain was rebuilt %zu times, each of which drains the device\n",
                                    device.swapchainRebuilds());
                    std::printf("%zu device allocations live holding %.0f MB, %zu and %.0f MB at peak, "
                                "of %u allocations the driver allows\n",
                                device.liveAllocations(), double(device.liveBytes()) / 1048576.0,
                                device.peakAllocations(), double(device.peakBytes()) / 1048576.0,
                                device.allocationLimit());
                    std::printf("%zu tree kinds at the bake, %zu in the end; %zu took a sibling's billboard, "
                                "%zu found none and %zu trees will never draw one\n",
                                kindsAtBake, treeKinds.size(), kindsSubstituted, billboardsMissed,
                                billboardInstancesMissed);
                    renderer.reportArenas();
                }
                break;
            }
        }

        // Give the view a few frames to settle, then take the picture and go.
        if (shotPath && ++frames == (benchFrames > 0 ? benchFrames - 1 : 5))
        {
            std::string shotError;
            // The per-second report has not had a chance to fire this early, and
            // a capture is worth nothing without the numbers behind it.
            std::printf("visible %zu of %zu (%zu too small, %zu occluded)\n", renderer.visibleInstances(),
                        renderer.instanceCount(), renderer.tooSmallInstances(), renderer.occludedInstances());
            loader.stop();
            std::printf("shot from %.0f %.0f %.0f looking %.2f %.2f radians\n", eye[0], eye[1], eye[2], yaw,
                        pitch);
            if (streaming)
                std::printf("%zu sectors arrived and %zu left; %zu tree kinds arrived after the billboard atlas "
                            "was baked and have none\n",
                            sectorsArrived, sectorsDeparted, billboardsMissed);
            if (device.capture(shotPath, &shotError))
                std::printf("wrote %s\n", shotPath);
            else
                std::printf("capture failed: %s\n", shotError.c_str());
            break;
        }
    }

    vkDeviceWaitIdle(device.device());
    loader.stop();
    renderer.stopProfiling();
    renderer.destroy(device);
    render::destroyTreeAtlas(device, treeAtlas);
    device.destroy();
    return 0;
}
