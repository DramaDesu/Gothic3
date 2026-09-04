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
#include "genome/collision.h"
#include "physics/world.h"
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

    // Opened after the flags are read, which happens below this - see where it
    // is filled in.

    // The game's own collision, per mesh name. Converted to an ordinary mesh so
    // that it can be drawn by the machinery that already exists, and scaled by
    // a hundred on the way: collision is in metres and everything drawn is in
    // centimetres.
    std::unique_ptr<genome::PakArchive> collisionArchive;
    std::map<std::string, genome::Mesh *> collisionOf;
    std::size_t collisionFound = 0, collisionMissing = 0, collisionTriangles = 0;
    // How the entity's own reference fares against the rule that stands in for
    // it: how many placements name a file, how many of those names resolve, and
    // how often the name and the rule disagree about which mesh it is.
    std::size_t namedPlacements = 0, namedShapes = 0, namedResolved = 0, namedDiffers = 0;
    std::set<std::string> namedUnresolved, namedDisagreements;
    // Every shape by kind, because a placement whose collision is a primitive
    // gets nothing from the name rule at all.
    std::map<int, std::size_t> shapesByKind;
    std::map<std::uint32_t, std::size_t> shapesByGroup;
    std::vector<std::string> collisionMissingNames;
    const auto collisionFor = [&](const std::string &meshName) -> genome::Mesh * {
        if (!collisionArchive)
            return nullptr;
        const auto cached = collisionOf.find(meshName);
        if (cached != collisionOf.end())
            return cached->second;

        std::string bare = meshName;
        const std::size_t slash = bare.find_last_of('/');
        if (slash != std::string::npos)
            bare = bare.substr(slash + 1);
        const std::size_t dot = bare.find_last_of('.');
        if (dot != std::string::npos)
            bare = bare.substr(0, dot);

        // The physics archive first, then the mesh archive, which carries 782
        // of its own - the landscape cells and the architecture among them.
        // Without the second, walls and floors come out with no collision at
        // all, which is exactly what the first look at this showed.
        std::string ignored;
        genome::CollisionMesh cooked;
        genome::Mesh *made = nullptr;

        // Half the archive names collision after the mesh it belongs to and the
        // other half appends _col, and the second half is where the walls and
        // the floors are. There is a third naming with the scale baked into the
        // name - _scx_0_6214_scy_1_0000_scz_1_0000 - because a cooked PhysX 2.x
        // triangle mesh cannot be scaled at use. We apply the world matrix,
        // scale included, so the unscaled mesh is the one to take.
        bool have = false;
        for (const char *suffix : {"", "_col", "_cv"})
        {
            have = genome::loadCollisionMesh(collisionArchive->read(bare + suffix + ".xnvmsh", &ignored), cooked,
                                             &ignored);
            if (have)
                break;
        }
        // Then the mesh archive, which carries 782 of its own.
        for (const char *suffix : {"", "_col", "_cv"})
        {
            if (have)
                break;
            std::string beside = meshName;
            const std::size_t swap = beside.find_last_of('.');
            if (swap != std::string::npos)
                beside = beside.substr(0, swap) + suffix + ".xnvmsh";
            have = genome::loadCollisionMesh(archive->read(beside, &ignored), cooked, &ignored);
        }
        if (have)
        {
            auto mesh = std::make_unique<genome::Mesh>();
            for (const genome::CollisionPart &part : cooked.parts)
            {
                genome::MeshElement element;
                element.positions.reserve(part.positions.size());
                for (const std::array<float, 3> &position : part.positions)
                    element.positions.push_back({position[0] * 100.0f, position[1] * 100.0f,
                                                 position[2] * 100.0f});
                // Flat normals from the triangles themselves: the cooked mesh
                // carries none, and the debug shading only needs enough to
                // read the shape.
                element.normals.assign(element.positions.size(), {0.0f, 1.0f, 0.0f});
                element.texCoords.assign(element.positions.size(), {0.0f, 0.0f});
                element.indices = part.indices;
                for (std::size_t at = 0; at + 2 < element.indices.size(); at += 3)
                {
                    const std::array<float, 3> &a = element.positions[element.indices[at]];
                    const std::array<float, 3> &b = element.positions[element.indices[at + 1]];
                    const std::array<float, 3> &c = element.positions[element.indices[at + 2]];
                    const std::array<float, 3> u{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
                    const std::array<float, 3> v{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
                    const std::array<float, 3> n{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                                                 u[0] * v[1] - u[1] * v[0]};
                    const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    if (length > 1e-6f)
                        for (int corner = 0; corner < 3; ++corner)
                            element.normals[element.indices[at + corner]] = {n[0] / length, n[1] / length,
                                                                            n[2] / length};
                }
                collisionTriangles += element.indices.size() / 3;
                mesh->elements.push_back(std::move(element));
            }
            made = mesh.get();
            ownedMeshes.push_back(std::move(mesh));
            ++collisionFound;
        }
        else
        {
            ++collisionMissing;
            if (collisionMissingNames.size() < 12)
                collisionMissingNames.push_back(bare);
        }

        collisionOf.emplace(meshName, made);
        return made;
    };

    // The shapes a tree definition declares, as a mesh in the grown tree's own
    // frame: metres to centimetres, and SpeedTree's z-up to the world's y-up.
    // The instance's scale is already in its world matrix, so nothing here is
    // per instance.
    std::map<std::string, genome::Mesh *> treeCollisionOf;
    std::size_t treeShapes = 0;
    const auto collisionForTree = [&](const std::string &key,
                                      const genome::SpeedTree &definition) -> genome::Mesh * {
        if (definition.collision.empty())
            return nullptr;
        const auto cached = treeCollisionOf.find(key);
        if (cached != treeCollisionOf.end())
            return cached->second;

        auto mesh = std::make_unique<genome::Mesh>();
        genome::MeshElement element;
        const auto vertex = [&](float x, float y, float z, float nx, float ny, float nz) {
            element.positions.push_back({x, y, z});
            element.normals.push_back({nx, ny, nz});
            element.texCoords.push_back({0.0f, 0.0f});
        };
        const auto quad = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
            element.indices.insert(element.indices.end(), {a, b, c, a, c, d});
        };

        for (const genome::CollisionPrimitive &shape : definition.collision)
        {
            // Metres to centimetres, and the third component is the one that
            // points up in a .spt.
            const float cx = shape.centre[0] * 100.0f;
            const float cy = shape.centre[2] * 100.0f;
            const float cz = shape.centre[1] * 100.0f;
            const std::uint32_t base = std::uint32_t(element.positions.size());

            if (shape.kind == genome::CollisionPrimitive::Kind::Box)
            {
                const float ex = shape.extent[0] * 100.0f;
                const float ey = shape.extent[2] * 100.0f;
                const float ez = shape.extent[1] * 100.0f;
                for (int corner = 0; corner < 8; ++corner)
                {
                    const float sx = (corner & 1) ? 1.0f : -1.0f;
                    const float sy = (corner & 2) ? 1.0f : -1.0f;
                    const float sz = (corner & 4) ? 1.0f : -1.0f;
                    vertex(cx + sx * ex, cy + sy * ey, cz + sz * ez, sx, sy, sz);
                }
                quad(base + 0, base + 2, base + 3, base + 1); // -z
                quad(base + 5, base + 7, base + 6, base + 4); // +z
                quad(base + 4, base + 6, base + 2, base + 0); // -x
                quad(base + 1, base + 3, base + 7, base + 5); // +x
                quad(base + 0, base + 1, base + 5, base + 4); // -y
                quad(base + 6, base + 7, base + 3, base + 2); // +y
                ++treeShapes;
                continue;
            }

            const int around = 12;
            if (shape.kind == genome::CollisionPrimitive::Kind::Cylinder)
            {
                // The trunk. It stands on its centre rather than being centred
                // on it: the base sits at the centre and the height goes up.
                const float radius = shape.radius * 100.0f;
                const float height = shape.height * 100.0f;
                for (int step = 0; step < around; ++step)
                {
                    const float angle = 6.2831853f * float(step) / float(around);
                    const float nx = std::cos(angle), nz = std::sin(angle);
                    vertex(cx + nx * radius, cy, cz + nz * radius, nx, 0.0f, nz);
                    vertex(cx + nx * radius, cy + height, cz + nz * radius, nx, 0.0f, nz);
                }
                for (int step = 0; step < around; ++step)
                {
                    const std::uint32_t a = base + std::uint32_t(step * 2);
                    const std::uint32_t b = base + std::uint32_t(((step + 1) % around) * 2);
                    quad(a, b, b + 1, a + 1);
                }
                ++treeShapes;
                continue;
            }

            // A canopy sphere.
            const int rings = 8;
            const float radius = shape.radius * 100.0f;
            for (int ring = 0; ring <= rings; ++ring)
            {
                const float phi = 3.14159265f * float(ring) / float(rings);
                for (int step = 0; step <= around; ++step)
                {
                    const float angle = 6.2831853f * float(step) / float(around);
                    const float nx = std::sin(phi) * std::cos(angle);
                    const float ny = std::cos(phi);
                    const float nz = std::sin(phi) * std::sin(angle);
                    vertex(cx + nx * radius, cy + ny * radius, cz + nz * radius, nx, ny, nz);
                }
            }
            for (int ring = 0; ring < rings; ++ring)
                for (int step = 0; step < around; ++step)
                {
                    const std::uint32_t a = base + std::uint32_t(ring * (around + 1) + step);
                    const std::uint32_t b = a + std::uint32_t(around + 1);
                    quad(a, a + 1, b + 1, b);
                }
            ++treeShapes;
        }

        if (element.indices.empty())
            return nullptr;
        mesh->elements.push_back(std::move(element));
        genome::Mesh *made = mesh.get();
        ownedMeshes.push_back(std::move(mesh));
        treeCollisionOf.emplace(key, made);
        return made;
    };

    // The primitives an entity authors on itself, as a mesh in its own frame.
    // Keyed by the numbers rather than by the object, since a box is a box:
    // hundreds of placements share a handful of distinct ones.
    std::map<std::string, genome::Mesh *> primitiveOf;
    std::size_t primitiveShapes = 0, primitivePlacements = 0;
    const auto primitiveMesh = [&](const std::vector<genome::CollisionShape> &shapes,
                                   const std::string &signature) -> genome::Mesh * {
        const auto cached = primitiveOf.find(signature);
        if (cached != primitiveOf.end())
            return cached->second;

        auto mesh = std::make_unique<genome::Mesh>();
        genome::MeshElement element;
        const auto place = [&](const genome::CollisionShape &shape, float x, float y, float z, float nx,
                               float ny, float nz) {
            // The rows of the orientation apply directly, as the entity
            // matrices' rows do.
            const auto &m = shape.orientation;
            element.positions.push_back({shape.centre[0] + m[0] * x + m[3] * y + m[6] * z,
                                         shape.centre[1] + m[1] * x + m[4] * y + m[7] * z,
                                         shape.centre[2] + m[2] * x + m[5] * y + m[8] * z});
            element.normals.push_back(
                {m[0] * nx + m[3] * ny + m[6] * nz, m[1] * nx + m[4] * ny + m[7] * nz,
                 m[2] * nx + m[5] * ny + m[8] * nz});
            element.texCoords.push_back({0.0f, 0.0f});
        };
        const auto quad = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
            element.indices.insert(element.indices.end(), {a, b, c, a, c, d});
        };

        for (const genome::CollisionShape &shape : shapes)
        {
            if (!shape.meshName.empty())
                continue;
            const std::uint32_t base = std::uint32_t(element.positions.size());

            if (shape.kind == genome::CollisionShape::Kind::Box)
            {
                for (int corner = 0; corner < 8; ++corner)
                {
                    const float sx = (corner & 1) ? 1.0f : -1.0f;
                    const float sy = (corner & 2) ? 1.0f : -1.0f;
                    const float sz = (corner & 4) ? 1.0f : -1.0f;
                    place(shape, sx * shape.extent[0], sy * shape.extent[1], sz * shape.extent[2], sx, sy, sz);
                }
                quad(base + 0, base + 2, base + 3, base + 1);
                quad(base + 5, base + 7, base + 6, base + 4);
                quad(base + 4, base + 6, base + 2, base + 0);
                quad(base + 1, base + 3, base + 7, base + 5);
                quad(base + 0, base + 1, base + 5, base + 4);
                quad(base + 6, base + 7, base + 3, base + 2);
                ++primitiveShapes;
                continue;
            }

            const int around = 12;
            if (shape.kind == genome::CollisionShape::Kind::Capsule)
            {
                // A capsule stands along its own y, centred: the cylinder is
                // the height and the caps are drawn as flat rings, which is
                // enough to read the shape.
                const float half = shape.height * 0.5f;
                for (int step = 0; step < around; ++step)
                {
                    const float angle = 6.2831853f * float(step) / float(around);
                    const float nx = std::cos(angle), nz = std::sin(angle);
                    place(shape, nx * shape.radius, -half, nz * shape.radius, nx, 0.0f, nz);
                    place(shape, nx * shape.radius, half, nz * shape.radius, nx, 0.0f, nz);
                }
                for (int step = 0; step < around; ++step)
                {
                    const std::uint32_t a = base + std::uint32_t(step * 2);
                    const std::uint32_t b = base + std::uint32_t(((step + 1) % around) * 2);
                    quad(a, b, b + 1, a + 1);
                }
                ++primitiveShapes;
                continue;
            }

            if (shape.kind != genome::CollisionShape::Kind::Sphere)
                continue;
            const int rings = 8;
            for (int ring = 0; ring <= rings; ++ring)
            {
                const float phi = 3.14159265f * float(ring) / float(rings);
                for (int step = 0; step <= around; ++step)
                {
                    const float angle = 6.2831853f * float(step) / float(around);
                    const float nx = std::sin(phi) * std::cos(angle);
                    const float ny = std::cos(phi);
                    const float nz = std::sin(phi) * std::sin(angle);
                    place(shape, nx * shape.radius, ny * shape.radius, nz * shape.radius, nx, ny, nz);
                }
            }
            for (int ring = 0; ring < rings; ++ring)
                for (int step = 0; step < around; ++step)
                {
                    const std::uint32_t a = base + std::uint32_t(ring * (around + 1) + step);
                    const std::uint32_t b = a + std::uint32_t(around + 1);
                    quad(a, a + 1, b + 1, b);
                }
            ++primitiveShapes;
        }

        if (element.indices.empty())
        {
            primitiveOf.emplace(signature, nullptr);
            return nullptr;
        }
        mesh->elements.push_back(std::move(element));
        genome::Mesh *made = mesh.get();
        ownedMeshes.push_back(std::move(mesh));
        primitiveOf.emplace(signature, made);
        return made;
    };

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
    float specularStrength = 1.0f, specularPower = 32.0f;
    // Runs the cull several times a frame. The cost of the extra passes is the
    // warm-cache cost; the difference from the first is what the memory costs.
    int cullRepeat = 1;
    // The card is the bottleneck now, so the pixel count is a first-class
    // parameter rather than a constant.
    int windowWidth = 1280, windowHeight = 720;
    // Threads the cull runs on, the caller included. Zero leaves the default.
    int cullThreads = 0;
    // The archive of cooked collision meshes, and what to draw: 0 the world,
    // 1 the world with its collision over it, 2 the collision alone.
    int collisionArgument = 0;
    int collisionView = 0;
    // How high the eye rides above the ground when walking, in world units.
    // A Gothic 3 character is about 180 tall.
    float walkHeight = 0.0f;
    // Hold forward for a headless run, so walking into things can be
    // measured without anyone at the keyboard.
    bool walkForward = false;
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
        if (std::string(argv[index]) == "--collision" && hasValue)
            collisionArgument = index + 1;
        if (std::string(argv[index]) == "--walk-forward")
        {
            walkForward = true;
            if (walkHeight <= 0.0f)
                walkHeight = 180.0f;
        }
        if (std::string(argv[index]) == "--walk")
            walkHeight = hasValue && argv[index + 1][0] != '-' ? float(std::atof(argv[index + 1])) : 180.0f;
        if (std::string(argv[index]) == "--collision-view" && hasValue)
            collisionView = std::atoi(argv[index + 1]);
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
        if (std::string(argv[index]) == "--specular" && index + 2 < argc)
        {
            specularStrength = float(std::atof(argv[index + 1]));
            specularPower = float(std::atof(argv[index + 2]));
        }
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

    // The collision archive, now that the flag naming it has been read.
    if (collisionArgument != 0)
    {
        std::string why;
        collisionArchive = genome::PakArchive::open(argv[collisionArgument], &why);
        if (!collisionArchive)
            std::printf("warning: no collision archive: %s\n", why.c_str());
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
                std::map<std::string, std::size_t> collisionBatchOf;
                std::map<std::string, std::size_t> treeBatchOf;
                // Per sector, like the two above: the mesh is shared across the
                // world but a batch index only means anything in the sector
                // whose batch vector it indexes.
                std::map<std::string, std::size_t> treeCollisionBatchOf;
                std::map<std::string, std::size_t> primitiveBatchOf;

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

                    // The primitives the entity authors on itself. They are
                    // handled here rather than beside the cooked meshes because
                    // they belong to the placement, not to its visual.
                    if (!placement.shapes.empty())
                    {
                        char number[64];
                        std::string signature;
                        for (const genome::CollisionShape &shape : placement.shapes)
                        {
                            if (!shape.meshName.empty())
                                continue;
                            std::snprintf(number, sizeof(number), "%d/%.2f/%.2f/%.2f/%.2f/%.2f/%.2f/%.2f/%.2f|",
                                          int(shape.kind), shape.centre[0], shape.centre[1], shape.centre[2],
                                          shape.extent[0], shape.extent[1], shape.extent[2], shape.radius,
                                          shape.height);
                            signature += number;
                            // The orientation belongs in the key too: the same
                            // box turned a different way is a different mesh.
                            for (float value : shape.orientation)
                            {
                                std::snprintf(number, sizeof(number), "%.3f,", value);
                                signature += number;
                            }
                        }
                        if (!signature.empty())
                        {
                            auto known = primitiveBatchOf.find(signature);
                            if (known == primitiveBatchOf.end())
                            {
                                std::size_t at = std::size_t(-1);
                                if (genome::Mesh *shape = primitiveMesh(placement.shapes, signature))
                                {
                                    render::MeshInstances batch;
                                    batch.mesh = shape;
                                    batch.collision = true;
                                    batch.occludes = false;
                                    at = batches.size();
                                    batches.push_back(std::move(batch));
                                }
                                known = primitiveBatchOf.emplace(signature, at).first;
                            }
                            if (known->second != std::size_t(-1))
                            {
                                ++primitivePlacements;
                                batches[known->second].transforms.push_back(placement.world);
                                batches[known->second].bounds.push_back(
                                    {placement.boundsMin[0], placement.boundsMin[1], placement.boundsMin[2],
                                     placement.boundsMax[0], placement.boundsMax[1], placement.boundsMax[2]});
                            }
                        }
                    }

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

                            const auto shape = collisionBatchOf.find(placement.meshName);
                            if (shape != collisionBatchOf.end())
                            {
                                batches[shape->second].transforms.push_back(placement.world);
                                batches[shape->second].bounds.push_back(
                                    {placement.boundsMin[0], placement.boundsMin[1], placement.boundsMin[2],
                                     placement.boundsMax[0], placement.boundsMax[1], placement.boundsMax[2]});
                            }
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
                    // What the entity itself says it collides with, checked
                    // against the archive rather than believed.
                    if (!placement.shapes.empty())
                    {
                        bool named = false;
                        for (const genome::CollisionShape &shape : placement.shapes)
                        {
                            ++shapesByKind[int(shape.kind)];
                            ++shapesByGroup[shape.group];
                            if (shape.meshName.empty())
                                continue;
                            named = true;
                            ++namedShapes;

                            std::string bare = shape.meshName;
                            const std::size_t dot = bare.find_last_of('.');
                            if (dot != std::string::npos)
                                bare = bare.substr(0, dot);

                            std::string why;
                            bool there = false;
                            if (collisionArchive && !collisionArchive->read(bare + ".xnvmsh", &why).empty())
                                there = true;
                            else if (!archive->read(bare + ".xnvmsh", &why).empty())
                                there = true;
                            if (there)
                                ++namedResolved;
                            else if (namedUnresolved.size() < 8)
                                namedUnresolved.insert(shape.meshName);

                            // The suffix rule's answer for this placement, for
                            // comparison: the visual's stem, bare or with _col.
                            std::string guess = placement.meshName;
                            const std::size_t slash = guess.find_last_of('/');
                            if (slash != std::string::npos)
                                guess = guess.substr(slash + 1);
                            const std::size_t guessDot = guess.find_last_of('.');
                            if (guessDot != std::string::npos)
                                guess = guess.substr(0, guessDot);

                            std::string lowered = bare, loweredGuess = guess;
                            for (char &c : lowered)
                                c = char(std::tolower(static_cast<unsigned char>(c)));
                            for (char &c : loweredGuess)
                                c = char(std::tolower(static_cast<unsigned char>(c)));
                            if (lowered != loweredGuess && lowered != loweredGuess + "_col" &&
                                lowered != loweredGuess + "_cv")
                            {
                                ++namedDiffers;
                                if (namedDisagreements.size() < 8)
                                    namedDisagreements.insert(guess + "  ->  " + bare);
                            }
                        }
                        if (named)
                            ++namedPlacements;
                    }

                    batchOf.emplace(placement.meshName, batches.size());
                    batches.push_back(std::move(batch));
                    ++placed;

                    // The same object's collision, in its own batch with the
                    // same transform. Its index is remembered so that later
                    // placements of the same mesh extend it too.
                    if (genome::Mesh *cooked = collisionFor(placement.meshName))
                    {
                        render::MeshInstances shape;
                        shape.mesh = cooked;
                        shape.collision = true;
                        shape.occludes = false;
                        shape.transforms.push_back(placement.world);
                        shape.bounds.push_back(
                            {placement.boundsMin[0], placement.boundsMin[1], placement.boundsMin[2],
                             placement.boundsMax[0], placement.boundsMax[1], placement.boundsMax[2]});
                        collisionBatchOf.emplace(placement.meshName, batches.size());
                        batches.push_back(std::move(shape));
                    }
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
                        genome::Mesh *treeShape = nullptr;
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

                            // The shapes the definition declares, in their own
                            // batch. Keyed without the variant digit: the
                            // collision is a property of the definition and does
                            // not change with which tree grew from it.
                            treeShape = collisionForTree(key.substr(0, key.size() - 1), definition);
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

                        const std::string bare = key.substr(0, key.size() - 1);
                        // The definition is only read once for the whole world,
                        // so every sector after the first takes its shapes from
                        // the cache rather than from the load above.
                        if (treeShape == nullptr)
                        {
                            const auto cached = treeCollisionOf.find(bare);
                            if (cached != treeCollisionOf.end())
                                treeShape = cached->second;
                        }
                        if (treeShape != nullptr && treeCollisionBatchOf.find(bare) == treeCollisionBatchOf.end())
                        {
                            render::MeshInstances shape;
                            shape.mesh = treeShape;
                            shape.collision = true;
                            shape.occludes = false;
                            treeCollisionBatchOf.emplace(bare, batches.size());
                            batches.push_back(std::move(shape));
                        }
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

                    const auto shape = treeCollisionBatchOf.find(key.substr(0, key.size() - 1));
                    if (shape != treeCollisionBatchOf.end())
                    {
                        batches[shape->second].transforms.push_back(tree.world);
                        batches[shape->second].bounds.push_back(box);
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
            if (collisionArchive)
            {
                std::printf("collision: %zu meshes found, %zu without one, %zu triangles placed\n",
                            collisionFound, collisionMissing, collisionTriangles);
                for (const std::string &name : collisionMissingNames)
                    std::printf("  no collision: %s\n", name.c_str());
                if (namedShapes != 0)
                {
                    std::printf("named shapes: %zu placements name %zu files, %zu resolve, %zu differ from "
                                "the rule\n",
                                namedPlacements, namedShapes, namedResolved, namedDiffers);
                    for (const std::string &name : namedUnresolved)
                        std::printf("  names nothing: %s\n", name.c_str());
                    for (const std::string &pair : namedDisagreements)
                        std::printf("  rule vs name: %s\n", pair.c_str());
                    static const char *kindNames[] = {"none",    "trimesh", "plane",      "box",
                                                      "capsule", "sphere",  "convexhull", "point"};
                    std::printf("  by kind:");
                    for (const auto &[kind, count] : shapesByKind)
                        std::printf(" %s x%zu", kind >= 0 && kind < 8 ? kindNames[kind] : "?", count);
                    std::printf("\n  by group:");
                    for (const auto &[group, count] : shapesByGroup)
                        std::printf(" %u x%zu", group, count);
                    std::printf("\n");
                }
                if (primitiveShapes != 0)
                    std::printf("primitives: %zu placements carry %zu distinct shape sets, %zu shapes\n",
                                primitivePlacements, primitiveOf.size(), primitiveShapes);
                if (treeShapes != 0)
                    std::printf("trees: %zu definitions declare %zu shapes\n", treeCollisionOf.size(),
                                treeShapes);
            }
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
    // What else the materials offer, and which shader classes they are, before
    // anything is built on top of them.
    std::map<std::string, std::size_t> slotsNamed, shaderClasses;
    std::set<std::string> specularSeen;
    std::size_t specularGrey = 0, specularColoured = 0, specularAlphaUsed = 0;
    std::size_t specularIsDiffuse = 0, specularIsOwn = 0;
    // Per material name, the normal map it resolved to. Kept beside the diffuse
    // cache rather than inside it because a material can name one without the
    // other.
    std::map<std::string, const genome::Image *> normalOf;
    // And per material, the specular map - which three times in four is the
    // diffuse image again, bound a second time rather than copied.
    std::map<std::string, const genome::Image *> specularOf;
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
        batch.speculars.clear();
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
                    ++shaderClasses[material.shaderClass.empty() ? std::string("(none)")
                                                                 : material.shaderClass];
                    for (std::size_t which = 0; which < genome::c_SlotCount; ++which)
                    {
                        const genome::Slot slot = genome::Slot(which);
                        const genome::Sampler *sampler = material.texture(slot);
                        if (!sampler)
                            continue;
                        const genome::TextureResolution named = genome::resolveTexture(*sampler, 0, exists);
                        ++slotsNamed[std::string(genome::slotName(slot)) +
                                     (named.fileName.empty() ? " (missing)" : "")];

                        // What a specular map actually holds: grey or coloured,
                        // and whether the alpha is doing anything.
                        // How often the specular slot just names the diffuse
                        // again: if it usually does, a third texture binding
                        // would mostly be a second copy of the first.
                        if (slot == genome::Slot::Specular && !named.fileName.empty())
                        {
                            const genome::Sampler *diffuseSampler = material.texture(genome::Slot::Diffuse);
                            const std::string diffuseFile =
                                diffuseSampler ? genome::resolveTexture(*diffuseSampler, 0, exists).fileName
                                               : std::string();
                            if (named.fileName == diffuseFile)
                                ++specularIsDiffuse;
                            else
                                ++specularIsOwn;
                        }
                        if (slot == genome::Slot::Specular && !named.fileName.empty() &&
                            specularSeen.insert(named.fileName).second)
                        {
                            genome::Image probe;
                            std::vector<std::uint8_t> decoded;
                            if (genome::loadImage(imageArchive->read(named.fileName, &ignored), probe, &ignored) &&
                                genome::decodeLevel(probe, 0, 0, decoded, &ignored))
                            {
                                std::uint8_t low[4] = {255, 255, 255, 255};
                                std::uint8_t high[4] = {0, 0, 0, 0};
                                double sums[4] = {0, 0, 0, 0};
                                std::size_t coloured = 0;
                                const std::size_t texels = decoded.size() / 4;
                                for (std::size_t at = 0; at < texels; ++at)
                                {
                                    for (int channel = 0; channel < 4; ++channel)
                                    {
                                        const std::uint8_t value = decoded[at * 4 + channel];
                                        low[channel] = std::min(low[channel], value);
                                        high[channel] = std::max(high[channel], value);
                                        sums[channel] += value;
                                    }
                                    const int spread = std::max({decoded[at * 4], decoded[at * 4 + 1],
                                                                 decoded[at * 4 + 2]}) -
                                                       std::min({decoded[at * 4], decoded[at * 4 + 1],
                                                                 decoded[at * 4 + 2]});
                                    coloured += spread > 16 ? 1 : 0;
                                }
                                if (texels != 0)
                                {
                                    specularGrey += coloured * 20 < texels ? 1 : 0;
                                    specularColoured += coloured * 20 >= texels ? 1 : 0;
                                    specularAlphaUsed += high[3] - low[3] > 32 ? 1 : 0;
                                    if (specularSeen.size() <= 4)
                                        std::printf("specular %s: r %3.0f [%3u..%3u] g %3.0f [%3u..%3u] "
                                                    "b %3.0f [%3u..%3u] a %3.0f [%3u..%3u], %.0f%% coloured\n",
                                                    named.fileName.c_str(), sums[0] / double(texels), low[0],
                                                    high[0], sums[1] / double(texels), low[1], high[1],
                                                    sums[2] / double(texels), low[2], high[2],
                                                    sums[3] / double(texels), low[3], high[3],
                                                    100.0 * double(coloured) / double(texels));
                                }
                            }
                        }
                    }
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
                    if (const genome::Sampler *shine = material.texture(genome::Slot::Specular))
                    {
                        const genome::TextureResolution resolvedShine = genome::resolveTexture(*shine, 0, exists);
                        if (!resolvedShine.fileName.empty())
                        {
                            const auto already = imageOfFile.find(resolvedShine.fileName);
                            if (already != imageOfFile.end())
                                specularOf.emplace(element.materialName, already->second);
                            else
                            {
                                auto image = std::make_unique<genome::Image>();
                                const genome::Image *loadedShine = nullptr;
                                if (genome::loadImage(imageArchive->read(resolvedShine.fileName, &ignored), *image,
                                                      &ignored))
                                {
                                    loadedShine = image.get();
                                    images.push_back(std::move(image));
                                }
                                imageOfFile.emplace(resolvedShine.fileName, loadedShine);
                                specularOf.emplace(element.materialName, loadedShine);
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
            const auto shone = specularOf.find(element.materialName);
            batch.speculars.push_back(shone != specularOf.end() ? shone->second : nullptr);
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
    std::printf("what the materials name:\n");
    for (const auto &[slot, count] : slotsNamed)
        std::printf("    %-24s %zu\n", slot.c_str(), count);
    std::printf("specular maps: %zu distinct, %zu greyscale, %zu coloured, %zu with a varying alpha\n",
                specularSeen.size(), specularGrey, specularColoured, specularAlphaUsed);
    std::printf("the specular slot names the diffuse again %zu times and its own file %zu times\n",
                specularIsDiffuse, specularIsOwn);
    std::printf("shader classes in front of the camera:\n");
    for (const auto &[name, count] : shaderClasses)
        std::printf("    %-40s %zu\n", name.c_str(), count);
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
    renderer.setSpecular(specularStrength, specularPower);
    if (cullThreads > 0)
        renderer.setCullThreads(unsigned(cullThreads));
    renderer.setCollisionView(collisionView);
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
    // The collision as something to ask questions of. Built from exactly the
    // batches the collision view draws, so the two cannot disagree about what
    // is solid, and rebuilt whole when the streamed set changes - it is a
    // fraction of what loading a sector already costs.
    physics::CollisionWorld solid;
    bool solidStale = false;
    const auto rebuildSolid = [&](bool report) {
        const auto start = now();
        solid.clear();
        const auto addFrom = [&](const std::vector<render::MeshInstances> &from) {
            for (const render::MeshInstances &batch : from)
            {
                if (!batch.collision || batch.mesh == nullptr)
                    continue;
                for (const auto &transform : batch.transforms)
                    solid.add(*batch.mesh, transform);
            }
        };
        addFrom(batches);
        for (const SectorContent &content : residentSectors)
            addFrom(content.batches);
        solid.build();
        if (report)
            std::printf("solid: %zu triangles placed from %zu distinct in %zu meshes, %zu cells, %zu references, built in %.0f ms\n",
                        solid.triangleCount(), solid.distinctTriangles(), solid.meshCount(),
                        solid.cellCount(), solid.referenceCount(), since(start) * 1000.0);
    };

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
    rebuildSolid(true);
    // Again, to show what a streaming rebuild costs: the first pass had to build
    // a tree for every distinct mesh and the second reuses them, which is the
    // difference between a one-off and a cost paid whenever the rectangle moves.
    rebuildSolid(true);
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

    if (walkHeight > 0.0f)
    {
        // A grid of drops over the streamed rectangle. The sky is well
        // above anything in the world, so a point that finds nothing found
        // a genuine hole rather than a short reach.
        const auto start = now();
        const float sky = 30000.0f;
        std::size_t asked = 0, answered = 0, visited = 0;
        float lowest = 1e9f, highest = -1e9f;
        for (int stepX = -20; stepX <= 20; ++stepX)
            for (int stepZ = -20; stepZ <= 20; ++stepZ)
            {
                const std::array<float, 3> probe{eye[0] + float(stepX) * 500.0f, sky,
                                                 eye[2] + float(stepZ) * 500.0f};
                float ground = 0.0f;
                ++asked;
                if (solid.groundBelow(probe, sky * 2.0f, ground))
                {
                    ++answered;
                    lowest = std::min(lowest, ground);
                    highest = std::max(highest, ground);
                }
                visited += solid.lastVisited();
            }
        const double seconds = since(start);
        std::printf("topmost surface: %zu of %zu points have one, %.0f..%.0f high, %.1f instances a query, %.0f us each\n",
                    answered, asked, double(lowest), double(highest), double(visited) / double(asked),
                    seconds / double(asked) * 1e6);

        // A lip a body could climb: neighbours on a fine grid that differ by
        // a stair's rise over a stride's width. Reported so a walk can be
        // aimed at one rather than sent hunting.
        {
            constexpr float c_Spacing = 25.0f;
            constexpr int c_Half = 60; // three thousand units across
            std::vector<float> heights(std::size_t(2 * c_Half + 1) * (2 * c_Half + 1), -1e9f);
            const auto sampleAt = [&](int ix, int iz) -> float & {
                return heights[std::size_t(ix + c_Half) * (2 * c_Half + 1) + (iz + c_Half)];
            };
            for (int ix = -c_Half; ix <= c_Half; ++ix)
                for (int iz = -c_Half; iz <= c_Half; ++iz)
                {
                    float ground = 0.0f;
                    const std::array<float, 3> probe{eye[0] + float(ix) * c_Spacing, eye[1] + 400.0f,
                                                     eye[2] + float(iz) * c_Spacing};
                    if (solid.groundBelow(probe, 2000.0f, ground))
                        sampleAt(ix, iz) = ground;
                }

            float bestRise = 0.0f;
            std::array<float, 3> bestAt{};
            float bestYaw = 0.0f;
            std::size_t lips = 0;
            for (int ix = -c_Half; ix < c_Half; ++ix)
                for (int iz = -c_Half; iz < c_Half; ++iz)
                {
                    const float here = sampleAt(ix, iz);
                    if (here < -1e8f)
                        continue;
                    for (int side = 0; side < 2; ++side)
                    {
                        const float there = side == 0 ? sampleAt(ix + 1, iz) : sampleAt(ix, iz + 1);
                        if (there < -1e8f)
                            continue;
                        const float rise = there - here;
                        // A stair, not a cliff and not a slope.
                        if (rise < 12.0f || rise > 50.0f)
                            continue;

                        // Halfway between the two samples. On a ramp it
                        // lands halfway up; on a step it is still at the
                        // bottom or already at the top. Only the second
                        // is something to climb.
                        float middle = 0.0f;
                        const std::array<float, 3> between{
                            eye[0] + (float(ix) + (side == 0 ? 0.5f : 0.0f)) * c_Spacing,
                            here + 200.0f,
                            eye[2] + (float(iz) + (side == 1 ? 0.5f : 0.0f)) * c_Spacing};
                        if (!solid.groundBelow(between, 400.0f, middle))
                            continue;
                        const float part = (middle - here) / rise;
                        if (part > 0.25f && part < 0.75f)
                            continue; // a ramp

                        ++lips;
                        if (rise <= bestRise)
                            continue;
                        bestRise = rise;
                        bestAt = {eye[0] + float(ix) * c_Spacing, here,
                                  eye[2] + float(iz) * c_Spacing};
                        // Face the higher neighbour: yaw 0 looks along +z
                        // and yaw 90 along +x.
                        bestYaw = side == 0 ? 90.0f : 0.0f;
                    }
                }
            std::printf("steps: %zu within 1500 of the camera; the tallest rises %.0f\n",
                        lips, double(bestRise));
            if (lips != 0)
                std::printf("  to meet it: --camera %.0f %.0f %.0f %.0f 0\n",
                            double(bestAt[0] - (bestYaw > 45.0f ? 150.0f : 0.0f)),
                            double(bestAt[1] + 200.0f),
                            double(bestAt[2] - (bestYaw > 45.0f ? 0.0f : 150.0f)), double(bestYaw));
        }

        // Every surface under the camera in turn, by starting the next probe
        // just under the last hit. One probe from the sky finds the roof and
        // says nothing about what a character could stand on.
        float from = sky;
        for (int layer = 0; layer < 12; ++layer)
        {
            float here = 0.0f;
            std::array<float, 3> face{};
            if (!solid.groundBelow({eye[0], from, eye[2]}, sky * 2.0f, here, &face))
                break;
            std::printf("  surface %d: %.0f, %s the eye by %.0f, facing %.2f %.2f %.2f\n", layer,
                        double(here), here > eye[1] ? "above" : "below",
                        double(std::fabs(eye[1] - here)), double(face[0]), double(face[1]),
                        double(face[2]));
            from = here - 1.0f;
        }
    }
    // The character, when walking. Gothic's unit is a centimetre, so these are
    // a person: a metre eighty tall, seventy across, eyes a little below the
    // top of the head.
    constexpr float c_BodyHeight = 180.0f;
    constexpr float c_BodyRadius = 35.0f;
    constexpr float c_EyeHeight = 165.0f;
    constexpr float c_Gravity = 981.0f;   // a centimetre is a centimetre
    constexpr float c_WalkSpeed = 350.0f; // per second
    constexpr float c_JumpSpeed = 420.0f;
    // Steeper than this is a wall to stand on, however level its normal looks:
    // about fifty degrees.
    constexpr float c_StandableY = 0.64f;
    // Measured, not guessed. The near-vertical faces of the game's own stair
    // meshes rise a median 44 to 52 with a ninetieth percentile of 64, so a
    // capsule of radius 35 does not ride over them by itself and the threshold
    // has to clear a real tread. Taller than this should be a climb.
    constexpr float c_StepHeight = 70.0f;
    std::array<float, 3> feet{eye[0], eye[1] - c_EyeHeight, eye[2]};
    float fallSpeed = 0.0f;
    bool onGround = false;
    bool landed = false;
    double asked = 0.0, covered = 0.0;
    std::size_t walkFrames = 0;
    std::size_t stepTried = 0, stepTaken = 0, stepDown = 0;
    float stepLift = 0.0f, stepBiggest = 0.0f;
    std::size_t stepNoGround = 0, stepNoLift = 0, stepNoGain = 0;
    std::vector<physics::CollisionWorld::Contact> contacts;

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
    // What a sector holds once its instances are counted rather than its
    // meshes: the number any collision structure would be built over.
    std::vector<std::size_t> sectorTriangles;
    std::vector<std::size_t> newTriangles;
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

        // What the player asked for. The spectator applies it outright; the
        // character asks the world first.
        std::array<float, 3> wish{0.0f, 0.0f, 0.0f};
        const auto move = [&](const std::array<float, 3> &direction, float scale) {
            for (int axis = 0; axis < 3; ++axis)
                wish[axis] += direction[axis] * scale;
        };
        if (walkForward)
            move(forward, speed);
        if (window.keyDown('W'))
            move(forward, speed);
        if (window.keyDown('S'))
            move(forward, -speed);
        if (window.keyDown('D'))
            move(right, speed);
        if (window.keyDown('A'))
            move(right, -speed);
        if (window.keyDown('E'))
            wish[1] += speed;
        if (window.keyDown('Q'))
            wish[1] -= speed;
        if (window.keyPressed('G'))
        {
            walkHeight = walkHeight > 0.0f ? 0.0f : c_BodyHeight;
            if (walkHeight > 0.0f)
            {
                // Take up the body where the camera is looking from, and let it
                // fall to whatever is under it.
                feet = {eye[0], eye[1] - c_EyeHeight, eye[2]};
                fallSpeed = 0.0f;
                onGround = false;
            }
            std::printf("walking: %s\n", walkHeight > 0.0f ? "on" : "off");
        }

        if (solidStale)
        {
            rebuildSolid(false);
            solidStale = false;
        }
        if (walkHeight <= 0.0f)
        {
            for (int axis = 0; axis < 3; ++axis)
                eye[axis] += wish[axis];
        }
        else
        {
            // Horizontal only, at a person's pace rather than the spectator's,
            // which scales with the world and would be a sprint indoors.
            std::array<float, 3> along{wish[0], 0.0f, wish[2]};
            const float length = std::sqrt(along[0] * along[0] + along[2] * along[2]);
            if (length > 1e-6f)
            {
                float pace = c_WalkSpeed * delta;
                if (window.keyDown(VK_SHIFT))
                    pace *= 3.0f;
                along[0] = along[0] / length * pace;
                along[2] = along[2] / length * pace;
            }

            if (onGround && window.keyPressed(' '))
            {
                fallSpeed = c_JumpSpeed;
                onGround = false;
            }
            fallSpeed -= c_Gravity * delta;
            // A body moving further than its own radius in one step can pass
            // through a wall before anything has a chance to push it back.
            fallSpeed = std::max(fallSpeed, -3000.0f);

            const std::array<float, 3> wasAt = feet;
            feet[0] += along[0];
            feet[2] += along[2];
            feet[1] += fallSpeed * delta;

            // Push out of everything the body is inside, and take the ground
            // from the normals that did the pushing. Four passes: one contact
            // resolved can press the body into another, and a corner is two
            // walls that each want the whole displacement.
            std::array<float, 3> support{0.0f, 0.0f, 0.0f};
            const auto settle = [&]() {
                bool grounded = false;
                for (int pass = 0; pass < 4; ++pass)
                {
                    contacts.clear();
                    const std::array<float, 3> low{feet[0], feet[1] + c_BodyRadius, feet[2]};
                    const std::array<float, 3> high{feet[0], feet[1] + c_BodyHeight - c_BodyRadius, feet[2]};
                    if (solid.capsuleContacts(low, high, c_BodyRadius, contacts) == 0)
                        break;

                    // The deepest contact first: resolving it often settles the
                    // shallow ones, and pushing along every normal at once
                    // over-corrects a corner.
                    const physics::CollisionWorld::Contact *worst = nullptr;
                    for (const auto &contact : contacts)
                        if (worst == nullptr || contact.depth > worst->depth)
                            worst = &contact;
                    if (worst == nullptr || worst->depth <= 0.001f)
                        break;
                    for (int axis = 0; axis < 3; ++axis)
                        feet[axis] += worst->normal[axis] * worst->depth;
                    if (worst->normal[1] > c_StandableY)
                    {
                        grounded = true;
                        support = worst->normal;
                    }
                }
                return grounded;
            };

            const bool wasGrounded = onGround;
            onGround = settle();

            // How far along the ground the body actually got. A step is worth
            // trying when it got much less than it asked for and it was walking
            // rather than falling.
            const auto travelled = [&](const std::array<float, 3> &from) {
                return std::sqrt((feet[0] - from[0]) * (feet[0] - from[0]) +
                                 (feet[2] - from[2]) * (feet[2] - from[2]));
            };
            const float wanted = std::sqrt(along[0] * along[0] + along[2] * along[2]);
            if (wasGrounded && wanted > 1e-3f && travelled(wasAt) < wanted * 0.9f)
            {
                // The same move from a step higher, then let it down again. Kept
                // only if it got further, so a wall costs one extra query pair
                // and changes nothing.
                ++stepTried;
                const std::array<float, 3> blocked = feet;
                const bool blockedGround = onGround;
                const float made = travelled(wasAt);

                feet = {wasAt[0] + along[0], wasAt[1] + c_StepHeight, wasAt[2] + along[2]};
                const bool steppedGround = settle();

                float ground = 0.0f;
                const bool landing =
                    solid.groundBelow({feet[0], feet[1] + c_StepHeight, feet[2]},
                                      c_StepHeight * 2.0f + c_BodyRadius, ground);
                if (landing)
                    feet[1] = ground;

                // A step counts only if the body ended up higher and further
                // along. Accepting it for the distance alone was the mistake
                // the measurement caught: pressed against a wall it accepted a
                // step that lowered the body, every frame, gaining nothing -
                // 198 accepted in one run and the largest lift among them zero.
                const float lift = landing ? ground - wasAt[1] : 0.0f;
                if (!landing)
                    ++stepNoGround;
                else if (lift <= 2.0f)
                    ++stepNoLift;
                else if (travelled(wasAt) <= made + 1.0f)
                    ++stepNoGain;
                if (!landing || lift <= 2.0f || travelled(wasAt) <= made + 1.0f)
                {
                    feet = blocked;
                    onGround = blockedGround;
                }
                else
                {
                    ++stepTaken;
                    stepLift += lift;
                    stepBiggest = std::max(stepBiggest, lift);
                    settle();
                    onGround = true;
                    fallSpeed = 0.0f;
                }
            }
            else if (wasGrounded && !onGround && fallSpeed <= 0.0f)
            {
                // Walking off a step should be a step down, not the start of a
                // fall: without this the body leaves the ground at every tread
                // and arrives at the bottom of a flight in the air.
                float ground = 0.0f;
                if (solid.groundBelow({feet[0], feet[1] + 1.0f, feet[2]}, c_StepHeight + 1.0f, ground))
                {
                    ++stepDown;
                    feet[1] = ground;
                    settle();
                    onGround = true;
                    fallSpeed = 0.0f;
                }
            }

            // Landing stops the fall; being pushed up by a slope does not turn
            // into a climb.
            if (onGround && fallSpeed < 0.0f)
                fallSpeed = 0.0f;

            // Say so once: that the body fell and stopped is the whole claim,
            // and a headless run can make it without anyone watching.
            if (onGround && !landed)
            {
                landed = true;
                // The clock is shadowed in this scope by the frame's own
                // timestamp, so it is read directly.
                const auto started = std::chrono::steady_clock::now();
                contacts.clear();
                solid.capsuleContacts({feet[0], feet[1] + c_BodyRadius, feet[2]},
                                      {feet[0], feet[1] + c_BodyHeight - c_BodyRadius, feet[2]},
                                      c_BodyRadius, contacts);
                const double micros =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() * 1e6;
                std::printf("landed at %.0f %.0f %.0f on a surface facing %.2f %.2f %.2f, "
                            "%zu contacts, %zu instances, %zu triangles, %.0f us\n",
                            double(feet[0]), double(feet[1]), double(feet[2]), double(support[0]),
                            double(support[1]), double(support[2]), contacts.size(), solid.lastVisited(),
                            solid.lastTested(), micros);
            }

            eye = {feet[0], feet[1] + c_EyeHeight, feet[2]};

            // What was asked for against what was covered. They part company at
            // a wall, which is the point of asking.
            if (walkForward)
            {
                asked += std::sqrt(along[0] * along[0] + along[2] * along[2]);
                covered += std::sqrt((feet[0] - wasAt[0]) * (feet[0] - wasAt[0]) +
                                     (feet[2] - wasAt[2]) * (feet[2] - wasAt[2]));
                if (++walkFrames % 60 == 0)
                    std::printf("walk %4zu: at %.0f %.0f %.0f, asked %.0f covered %.0f, %s, %zu tried %zu climbed %zu descended, lifted %.0f biggest %.0f, refused %zu no-ground %zu no-lift %zu no-gain\n",
                                walkFrames, double(feet[0]), double(feet[1]), double(feet[2]), asked,
                                covered, onGround ? "grounded" : "airborne", stepTried, stepTaken, stepDown,
                                double(stepLift), double(stepBiggest), stepNoGround, stepNoLift,
                                stepNoGain);
            }
        }
        if (window.keyPressed('O'))
            occlusion = !occlusion;
        // Off, over the world, or alone. Nothing is reloaded: the collision is
        // already there as batches and the cull decides which are wanted.
        if (window.keyPressed('C'))
            renderer.setCollisionView((renderer.collisionView() + 1) % 3);
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
                    solidStale = true;
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
                        // Placed triangles, not distinct ones: a crate used
                        // twenty times is one mesh to the renderer and twenty
                        // sets of triangles to anything that has to collide.
                        // And how much of it is geometry never seen before,
                        // which is what a per-mesh structure would actually
                        // have to build. The arena knows: it only grows for a
                        // mesh that was not already there.
                        const std::size_t indicesBefore = renderer.triangleCount();
                        std::size_t placed = 0;
                        for (const render::MeshInstances &batch : content.batches)
                        {
                            if (!batch.mesh)
                                continue;
                            std::size_t ofOne = 0;
                            for (const genome::MeshElement &element : batch.mesh->elements)
                                ofOne += element.indices.size() / 3;
                            placed += ofOne * batch.transforms.size();
                        }
                        sectorTriangles.push_back(placed);
                        newTriangles.push_back(renderer.triangleCount() - indicesBefore);

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
                if (walkForward)
                    std::printf("walked: asked for %.0f, covered %.0f, ended at %.0f %.0f %.0f, %s\n",
                                asked, covered, double(feet[0]), double(feet[1]), double(feet[2]),
                                onGround ? "on the ground" : "in the air");
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
                    if (!sectorTriangles.empty())
                    {
                        std::vector<std::size_t> sorted = sectorTriangles;
                        std::sort(sorted.begin(), sorted.end());
                        std::size_t total = 0;
                        for (std::size_t one : sorted)
                            total += one;
                        std::printf("a sector brings %zu triangles at the median, %zu at the 90th, %zu at most, "
                                    "%zu at least; %zu across %zu arrivals\n",
                                    sorted[sorted.size() / 2], sorted[sorted.size() * 9 / 10], sorted.back(),
                                    sorted.front(), total, sorted.size());
                        std::vector<std::size_t> fresh = newTriangles;
                        std::sort(fresh.begin(), fresh.end());
                        std::size_t freshTotal = 0;
                        for (std::size_t one : fresh)
                            freshTotal += one;
                        std::printf("of those %zu are triangles never seen before: %zu at the median, %zu at the "
                                    "90th, %zu at most\n",
                                    freshTotal, fresh[fresh.size() / 2], fresh[fresh.size() * 9 / 10], fresh.back());
                    }
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
