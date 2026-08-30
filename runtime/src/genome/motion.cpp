#include "motion.h"

#include <algorithm>
#include <cmath>

namespace genome
{
namespace
{

constexpr std::uint32_t c_ChunkMotionPart = 1;
constexpr std::uint32_t c_ChunkAnim = 2;

using Quat = std::array<float, 4>;
using Vec3 = std::array<float, 3>;

Quat multiply(const Quat &a, const Quat &b)
{
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

Vec3 rotate(const Quat &q, const Vec3 &v)
{
    const Vec3 u{q[0], q[1], q[2]};
    const float s = q[3];
    const float dot = u[0] * v[0] + u[1] * v[1] + u[2] * v[2];
    const Vec3 cross{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    const float uu = u[0] * u[0] + u[1] * u[1] + u[2] * u[2];
    Vec3 out{};
    for (int axis = 0; axis < 3; ++axis)
        out[axis] = 2.0f * dot * u[axis] + (s * s - uu) * v[axis] + 2.0f * s * cross[axis];
    return out;
}

Quat normalise(Quat q)
{
    const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (length > 0.0f)
        for (float &component : q)
            component /= length;
    return q;
}

// Shortest-path linear blend: keys can sit in opposite hemispheres, which would
// otherwise spin a bone the long way round.
Quat blend(const Quat &a, Quat b, float t)
{
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (dot < 0.0f)
        for (float &component : b)
            component = -component;
    Quat out{};
    for (int index = 0; index < 4; ++index)
        out[index] = a[index] + (b[index] - a[index]) * t;
    return normalise(out);
}

Matrix4 compose(const Transform &transform)
{
    const float x = transform.rotation[0], y = transform.rotation[1], z = transform.rotation[2],
                w = transform.rotation[3];
    const auto &s = transform.scale;
    Matrix4 m{};
    m[0] = (1.0f - 2.0f * (y * y + z * z)) * s[0];
    m[1] = (2.0f * (x * y + z * w)) * s[0];
    m[2] = (2.0f * (x * z - y * w)) * s[0];
    m[4] = (2.0f * (x * y - z * w)) * s[1];
    m[5] = (1.0f - 2.0f * (x * x + z * z)) * s[1];
    m[6] = (2.0f * (y * z + x * w)) * s[1];
    m[8] = (2.0f * (x * z + y * w)) * s[2];
    m[9] = (2.0f * (y * z - x * w)) * s[2];
    m[10] = (1.0f - 2.0f * (x * x + y * y)) * s[2];
    m[12] = transform.position[0];
    m[13] = transform.position[1];
    m[14] = transform.position[2];
    m[15] = 1.0f;
    return m;
}

Matrix4 multiply(const Matrix4 &a, const Matrix4 &b)
{
    Matrix4 out{};
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

// A node is a helper the engine folds away when its name ends in _ROOT or _END
// and carries more than two underscore-separated parts: "Orc_ROOT" survives,
// "Orc_Left_Arm_Arm_ROOT" does not.
bool isHelper(const std::string &name)
{
    const auto endsWith = [&](const char *suffix) {
        const std::size_t length = std::strlen(suffix);
        return name.size() > length && name.compare(name.size() - length, length, suffix) == 0;
    };
    if (!endsWith("_ROOT") && !endsWith("_END"))
        return false;
    return std::count(name.begin(), name.end(), '_') > 2;
}

} // namespace

int Skeleton::find(std::string_view name) const
{
    for (std::size_t index = 0; index < bones.size(); ++index)
        if (bones[index].name == name)
            return static_cast<int>(index);
    return -1;
}

Skeleton buildSkeleton(const Actor &actor)
{
    Skeleton skeleton;
    std::vector<int> mapping(actor.nodes.size(), -1);

    for (std::size_t index = 0; index < actor.nodes.size(); ++index)
    {
        if (isHelper(actor.nodes[index].name))
            continue;
        mapping[index] = static_cast<int>(skeleton.bones.size());
        Skeleton::Bone bone;
        bone.name = actor.nodes[index].name;
        bone.originalNode = static_cast<int>(index);
        bone.rest.position = actor.nodes[index].position;
        bone.rest.rotation = actor.nodes[index].rotation;
        bone.rest.scale = actor.nodes[index].scale;
        skeleton.bones.push_back(std::move(bone));
    }

    // Walking up through dropped helpers folds their transforms into the child,
    // which is exactly what the engine does before animating.
    for (Skeleton::Bone &bone : skeleton.bones)
    {
        int parent = actor.nodes[bone.originalNode].parent;
        while (parent >= 0 && mapping[parent] < 0)
        {
            const Node &helper = actor.nodes[parent];
            bone.rest.position = [&] {
                const Vec3 rotated = rotate(helper.rotation, bone.rest.position);
                return Vec3{helper.position[0] + rotated[0], helper.position[1] + rotated[1],
                            helper.position[2] + rotated[2]};
            }();
            bone.rest.rotation = normalise(multiply(helper.rotation, bone.rest.rotation));
            parent = helper.parent;
        }
        bone.parent = parent >= 0 ? mapping[parent] : -1;
    }

    return skeleton;
}

const MotionPart *Motion::find(std::string_view name) const
{
    for (const MotionPart &part : parts)
        if (part.name == name)
            return &part;
    return nullptr;
}

bool loadMotion(const std::vector<std::uint8_t> &bytes, Motion &motion, std::string *error)
{
    const auto fail = [&](const char *reason) {
        if (error)
            *error = reason;
        return false;
    };

    Reader reader(bytes);
    const StringTable strings = StringTable::sniff(reader);
    if (!reader.ok() || !strings.wrapped())
        return fail("motion is not a wrapped Genome file");

    const std::uint16_t version = reader.u16();
    if (version != 5)
        return fail("unexpected motion version");

    reader.skip(4);  // resource size
    reader.skip(4);  // priority, junk
    reader.skip(8);  // file time
    reader.skip(4);  // file size
    reader.skip(8);  // second file time, present from version 3

    const std::uint16_t effectCount = reader.u16();
    std::vector<std::pair<std::uint16_t, std::string>> effects;
    for (std::uint16_t index = 0; index < effectCount && reader.ok(); ++index)
    {
        const std::uint16_t frame = reader.u16();
        effects.emplace_back(frame, strings.entry(reader));
    }

    reader.skip(4); // payload size
    if (!reader.match("LMA ", 4))
        return fail("missing motion block");
    reader.skip(4);
    reader.skip(2); // versions
    reader.skip(1); // actor flag, zero for a motion

    while (reader.ok() && reader.remaining() >= 12 && reader.tell() + 12 <= strings.payloadEnd())
    {
        const std::uint32_t id = reader.u32();
        const std::uint32_t size = reader.u32();
        reader.u32(); // chunk version
        const std::size_t chunkEnd = reader.tell() + size;
        if (chunkEnd > strings.payloadEnd())
            break;

        if (id == c_ChunkMotionPart)
        {
            MotionPart part;
            reader.array(part.pose.position.data(), 3);
            reader.array(part.pose.rotation.data(), 4);
            reader.array(part.pose.scale.data(), 3);
            reader.skip(40); // "bind pose" fields, uninitialised in every file
            part.name = reader.string32();
            motion.parts.push_back(std::move(part));
        }
        else if (id == c_ChunkAnim && !motion.parts.empty())
        {
            MotionTrack track;
            const std::uint32_t keyCount = reader.u32();
            reader.skip(1); // interpolation, always linear in shipping data
            track.channel = static_cast<char>(reader.u8());
            reader.skip(2); // junk, not zero

            if (keyCount > (1u << 20))
                return fail("implausible key count");
            track.times.reserve(keyCount);
            track.values.reserve(keyCount);
            for (std::uint32_t key = 0; key < keyCount && reader.ok(); ++key)
            {
                const float time = reader.f32();
                std::array<float, 4> value{0.0f, 0.0f, 0.0f, 1.0f};
                if (track.channel == 'R')
                    reader.array(value.data(), 4);
                else
                    reader.array(value.data(), 3);
                track.times.push_back(time);
                track.values.push_back(value);
                motion.duration = std::max(motion.duration, time);
            }
            motion.parts.back().tracks.push_back(std::move(track));
        }

        reader.seek(chunkEnd);
    }

    for (const auto &[frame, name] : effects)
        motion.effects.emplace_back(static_cast<float>(frame) / motion.frameRate, name);

    return reader.ok() && !motion.parts.empty();
}

std::vector<Matrix4> samplePose(const Skeleton &skeleton, const Motion &motion, float time)
{
    std::vector<Transform> locals(skeleton.bones.size());

    for (std::size_t index = 0; index < skeleton.bones.size(); ++index)
    {
        const Skeleton::Bone &bone = skeleton.bones[index];
        Transform &local = locals[index];
        local = bone.rest;

        const MotionPart *part = motion.find(bone.name);
        if (!part)
            continue;

        // A part without tracks still overrides the rest pose with its own.
        local.position = part->pose.position;
        local.rotation = part->pose.rotation;
        local.scale = part->pose.scale;

        for (const MotionTrack &track : part->tracks)
        {
            if (track.times.empty())
                continue;

            // Hold before the first key and after the last.
            std::size_t next = 0;
            while (next < track.times.size() && track.times[next] < time)
                ++next;

            std::array<float, 4> value{};
            if (next == 0)
                value = track.values.front();
            else if (next >= track.times.size())
                value = track.values.back();
            else
            {
                const float span = track.times[next] - track.times[next - 1];
                const float t = span > 0.0f ? (time - track.times[next - 1]) / span : 0.0f;
                if (track.channel == 'R')
                    value = blend(track.values[next - 1], track.values[next], t);
                else
                    for (int axis = 0; axis < 3; ++axis)
                        value[axis] =
                            track.values[next - 1][axis] + (track.values[next][axis] - track.values[next - 1][axis]) * t;
            }

            if (track.channel == 'R')
                local.rotation = normalise(value);
            else if (track.channel == 'P')
                local.position = {value[0], value[1], value[2]};
            else if (track.channel == 'S')
                local.scale = {value[0], value[1], value[2]};
        }
    }

    // Bones are stored in the actor's own order, where a parent may follow its
    // child, so compose depth-first instead of in array order.
    std::vector<Matrix4> globals(skeleton.bones.size());
    std::vector<bool> resolved(skeleton.bones.size(), false);
    const auto resolve = [&](auto &&self, std::size_t index) -> void {
        if (resolved[index])
            return;
        resolved[index] = true;
        const Matrix4 local = compose(locals[index]);
        const int parent = skeleton.bones[index].parent;
        if (parent < 0)
            globals[index] = local;
        else
        {
            self(self, static_cast<std::size_t>(parent));
            globals[index] = multiply(globals[parent], local);
        }
    };
    for (std::size_t index = 0; index < skeleton.bones.size(); ++index)
        resolve(resolve, index);

    return globals;
}

std::vector<Matrix4> skinningMatrices(const Actor &actor, const Skeleton &skeleton, const std::vector<Matrix4> &pose)
{
    std::vector<Matrix4> matrices(actor.nodes.size());
    for (Matrix4 &matrix : matrices)
        matrix = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    for (std::size_t index = 0; index < skeleton.bones.size() && index < pose.size(); ++index)
    {
        const int original = skeleton.bones[index].originalNode;
        if (original >= 0 && static_cast<std::size_t>(original) < matrices.size())
            matrices[original] = multiply(pose[index], actor.nodes[original].inverseBind);
    }
    return matrices;
}

void skinVertices(const Actor &actor, const std::vector<Matrix4> &skinning, std::vector<std::array<float, 3>> &out)
{
    out.clear();
    out.reserve(actor.vertexCount());

    for (const ActorSubmesh &submesh : actor.submeshes)
    {
        for (const ActorVertex &vertex : submesh.vertices)
        {
            std::array<float, 3> position{};
            float total = 0.0f;

            if (vertex.originalVertex < actor.influences.size())
            {
                for (const SkinInfluence &influence : actor.influences[vertex.originalVertex])
                {
                    if (influence.node >= skinning.size() || influence.weight <= 0.0f)
                        continue;
                    const Matrix4 &m = skinning[influence.node];
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        const float value = m[0 * 4 + axis] * vertex.position[0] + m[1 * 4 + axis] * vertex.position[1] +
                                            m[2 * 4 + axis] * vertex.position[2] + m[3 * 4 + axis];
                        position[axis] += value * influence.weight;
                    }
                    total += influence.weight;
                }
            }

            if (total > 0.0f)
                for (float &component : position)
                    component /= total;
            else
                position = vertex.position;

            out.push_back(position);
        }
    }
}

} // namespace genome
