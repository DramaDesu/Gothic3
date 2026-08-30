#include "actor.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace genome
{
namespace
{

// EmotionFX chunk ids that carry something we need.
constexpr std::uint32_t c_ChunkNode = 0;
constexpr std::uint32_t c_ChunkMesh = 3;
constexpr std::uint32_t c_ChunkSkinningInfo = 4;
constexpr std::uint32_t c_ChunkSceneInfo = 16;

Matrix4 identity()
{
    Matrix4 m{};
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    return m;
}

Matrix4 multiply(const Matrix4 &a, const Matrix4 &b)
{
    Matrix4 out{};
    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
                sum += a[k * 4 + row] * b[column * 4 + k];
            out[column * 4 + row] = sum;
        }
    }
    return out;
}

// Translation, rotation and uniform scale, in that order.
Matrix4 compose(const std::array<float, 3> &position, const std::array<float, 4> &q, const std::array<float, 3> &scale)
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    Matrix4 m{};
    m[0] = (1.0f - 2.0f * (y * y + z * z)) * scale[0];
    m[1] = (2.0f * (x * y + z * w)) * scale[0];
    m[2] = (2.0f * (x * z - y * w)) * scale[0];
    m[4] = (2.0f * (x * y - z * w)) * scale[1];
    m[5] = (1.0f - 2.0f * (x * x + z * z)) * scale[1];
    m[6] = (2.0f * (y * z + x * w)) * scale[1];
    m[8] = (2.0f * (x * z + y * w)) * scale[2];
    m[9] = (2.0f * (y * z - x * w)) * scale[2];
    m[10] = (1.0f - 2.0f * (x * x + y * y)) * scale[2];
    m[12] = position[0];
    m[13] = position[1];
    m[14] = position[2];
    m[15] = 1.0f;
    return m;
}

// Full 4x4 inverse. The transforms are affine and mostly rigid, but a handful of
// nodes carry mirror scales, so the general form avoids special cases.
Matrix4 inverse(const Matrix4 &m)
{
    Matrix4 inv{};
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    const float determinant = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (determinant == 0.0f)
        return identity();
    const float scale = 1.0f / determinant;
    for (float &value : inv)
        value *= scale;
    return inv;
}

// Strings inside chunks are length-prefixed with a 32-bit count and, unlike the
// container's strings, never go through the string table.
std::string chunkString(Reader &reader)
{
    return reader.string32();
}

void readNode(Reader &reader, Actor &actor)
{
    Node node;
    reader.array(node.position.data(), 3);
    reader.array(node.rotation.data(), 4);
    reader.skip(16); // scale orientation, irrelevant while scale is uniform
    reader.array(node.scale.data(), 3);
    reader.skip(12); // shear, zero everywhere
    node.name = chunkString(reader);
    node.parentName = chunkString(reader);
    actor.nodes.push_back(std::move(node));
}

bool readMesh(Reader &reader, Actor &actor)
{
    actor.meshNode = reader.u32();
    reader.u32(); // original vertex count, implied by the skinning table
    reader.u32(); // total vertices
    reader.u32(); // total indices
    const std::uint32_t submeshCount = reader.u32();
    const std::uint32_t uvSetCount = reader.u32();
    reader.skip(4); // collision flag plus padding

    if (!reader.ok() || submeshCount > 256 || uvSetCount > 8)
        return false;

    for (std::uint32_t index = 0; index < submeshCount && reader.ok(); ++index)
    {
        ActorSubmesh submesh;
        submesh.materialIndex = reader.u8();
        reader.skip(1); // per-submesh uv count, which the engine ignores
        reader.skip(2);
        const std::uint32_t indexCount = reader.u32();
        const std::uint32_t vertexCount = reader.u32();
        if (!reader.ok() || vertexCount > 1u << 22 || indexCount > 1u << 23)
            return false;

        submesh.vertices.resize(vertexCount);
        for (std::uint32_t vertex = 0; vertex < vertexCount && reader.ok(); ++vertex)
        {
            ActorVertex &out = submesh.vertices[vertex];
            out.originalVertex = reader.u32();
            reader.array(out.position.data(), 3);
            reader.array(out.normal.data(), 3);
            for (std::uint32_t uv = 0; uv < uvSetCount; ++uv)
            {
                std::array<float, 2> coord{};
                reader.array(coord.data(), 2);
                if (uv == 0)
                    out.texCoord = coord;
            }
        }

        submesh.indices.resize(indexCount);
        reader.array(submesh.indices.data(), indexCount);
        actor.submeshes.push_back(std::move(submesh));
    }
    return reader.ok();
}

// One record per original vertex, running until the chunk is exhausted: there is
// no count, so the chunk end is the terminator.
bool readSkinningInfo(Reader &reader, std::size_t chunkEnd, Actor &actor)
{
    reader.u32(); // mesh node, always the same as the mesh chunk's
    while (reader.ok() && reader.tell() < chunkEnd)
    {
        const std::uint8_t count = reader.u8();
        std::vector<SkinInfluence> influences;
        influences.reserve(count);
        for (std::uint8_t index = 0; index < count && reader.ok(); ++index)
        {
            SkinInfluence influence;
            influence.node = reader.u16();
            reader.skip(2); // uninitialised, not a high word
            influence.weight = reader.f32();
            influences.push_back(influence);
        }
        // Heaviest bones first, so a GPU path can simply keep the first four.
        std::sort(influences.begin(), influences.end(),
                  [](const SkinInfluence &a, const SkinInfluence &b) { return a.weight > b.weight; });
        actor.influences.push_back(std::move(influences));
    }
    return reader.ok();
}

} // namespace

std::size_t Actor::vertexCount() const
{
    std::size_t total = 0;
    for (const ActorSubmesh &submesh : submeshes)
        total += submesh.vertices.size();
    return total;
}

std::size_t Actor::triangleCount() const
{
    std::size_t total = 0;
    for (const ActorSubmesh &submesh : submeshes)
        total += submesh.indices.size() / 3;
    return total;
}

const Node *Actor::findNode(std::string_view name) const
{
    const auto match = std::find_if(nodes.begin(), nodes.end(), [&](const Node &node) { return node.name == name; });
    return match == nodes.end() ? nullptr : &*match;
}

void Actor::computeBounds(std::array<float, 3> &min, std::array<float, 3> &max) const
{
    min = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    max = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
           std::numeric_limits<float>::lowest()};
    for (const ActorSubmesh &submesh : submeshes)
    {
        for (const ActorVertex &vertex : submesh.vertices)
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                min[axis] = std::min(min[axis], vertex.position[axis]);
                max[axis] = std::max(max[axis], vertex.position[axis]);
            }
        }
    }
}

bool loadActor(const std::vector<std::uint8_t> &bytes, Actor &actor, std::string *error)
{
    const auto fail = [&](const char *reason) {
        if (error)
            *error = reason;
        return false;
    };

    Reader reader(bytes);
    const StringTable strings = StringTable::sniff(reader);
    if (!reader.ok() || !strings.wrapped())
        return fail("actor is not a wrapped Genome file");

    const std::uint16_t version = reader.u16();
    if (version != 54)
        return fail("unexpected actor version");

    reader.skip(4);  // resource size, zero in shipping data
    reader.skip(4);  // priority, uninitialised
    reader.skip(8);  // native file time
    reader.skip(4);  // native file size
    reader.skip(24); // bounding box, stored invalid: recomputed from vertices

    const std::uint32_t lookAtCount = reader.u32();
    reader.skip(static_cast<std::size_t>(lookAtCount) * 30);

    // Coarser LOD actors come first; the base actor is last and is the one we
    // want, so parse them all and keep the final one.
    const std::uint32_t lodActorCount = reader.u32();
    if (!reader.ok() || lodActorCount > 8)
        return fail("implausible lod actor count");

    for (std::uint32_t lod = 0; lod <= lodActorCount && reader.ok(); ++lod)
    {
        Actor parsed;

        if (!reader.match("gena", 4))
            return fail("missing actor block");
        reader.skip(4);
        reader.skip(2); // wrapper version
        const std::uint32_t payloadSize = reader.u32();
        const std::size_t streamEnd = reader.tell() + payloadSize;

        if (!reader.match("FXA ", 4))
            return fail("missing FXA block");
        reader.skip(4);
        reader.skip(2); // high and low version

        while (reader.ok() && reader.tell() + 12 <= streamEnd)
        {
            const std::uint32_t id = reader.u32();
            const std::uint32_t size = reader.u32();
            reader.u32(); // chunk version
            std::size_t chunkEnd = reader.tell() + size;
            if (chunkEnd > streamEnd)
                return fail("chunk overruns the actor block");

            switch (id)
            {
                case c_ChunkNode: readNode(reader, parsed); break;
                case c_ChunkSceneInfo:
                {
                    // Two shipped head actors declare this chunk smaller than its
                    // contents, so a size-driven walk desyncs and then reads a
                    // garbage id. Parse it and take whichever end is later.
                    reader.skip(8);
                    for (int index = 0; index < 3 && reader.ok(); ++index)
                        chunkString(reader);
                    if (reader.ok() && reader.tell() > chunkEnd)
                    {
                        reader.seek(reader.tell());
                        continue;
                    }
                    break;
                }
                case c_ChunkMesh:
                    if (!readMesh(reader, parsed))
                        return fail("bad mesh chunk");
                    break;
                case c_ChunkSkinningInfo:
                    if (!readSkinningInfo(reader, chunkEnd, parsed))
                        return fail("bad skinning chunk");
                    break;
                default: break;
            }
            reader.seek(chunkEnd);
        }
        reader.seek(streamEnd);

        // Material references live after the chunk stream, naming the real
        // .xshmat resources that submesh material indices point at.
        const std::uint32_t materialRefCount = reader.u32();
        for (std::uint32_t index = 0; index < materialRefCount && reader.ok(); ++index)
        {
            reader.skip(2); // lod index, always zero
            const std::uint16_t materialIndex = reader.u16();
            const std::string name = strings.entry(reader);
            if (parsed.materials.size() <= materialIndex)
                parsed.materials.resize(materialIndex + 1);
            parsed.materials[materialIndex] = name;
        }

        reader.skip(1); // container junk byte
        const std::uint32_t aoLodCount = reader.u32();
        for (std::uint32_t index = 0; index < aoLodCount && reader.ok(); ++index)
        {
            reader.skip(1);
            const std::uint32_t count = reader.u32();
            reader.skip(static_cast<std::size_t>(count) * 4); // ambient occlusion
            reader.skip(static_cast<std::size_t>(count) * 12); // tangents, no count of their own
        }

        if (lod == lodActorCount)
            actor = std::move(parsed);
    }

    if (!reader.ok())
        return fail("truncated actor");
    if (actor.nodes.empty())
        return fail("actor has no nodes");

    // Parents are named, not indexed, and the array is not sorted, so resolve in
    // one pass and compose in a second.
    for (Node &node : actor.nodes)
    {
        node.local = compose(node.position, node.rotation, node.scale);
        node.parent = -1;
        if (node.parentName.empty())
            continue;
        for (std::size_t index = 0; index < actor.nodes.size(); ++index)
        {
            if (actor.nodes[index].name == node.parentName)
            {
                node.parent = static_cast<int>(index);
                break;
            }
        }
    }

    std::vector<bool> resolved(actor.nodes.size(), false);
    const auto composeNode = [&](auto &&self, std::size_t index) -> void {
        if (resolved[index])
            return;
        Node &node = actor.nodes[index];
        if (node.parent < 0)
            node.globalBind = node.local;
        else
        {
            self(self, static_cast<std::size_t>(node.parent));
            node.globalBind = multiply(actor.nodes[node.parent].globalBind, node.local);
        }
        node.inverseBind = inverse(node.globalBind);
        resolved[index] = true;
    };
    for (std::size_t index = 0; index < actor.nodes.size(); ++index)
        composeNode(composeNode, index);

    return true;
}

} // namespace genome
