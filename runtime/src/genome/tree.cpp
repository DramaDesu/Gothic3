#include "tree.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace genome
{
namespace
{

// The definition gives a size in its own units; the world gives heights in
// centimetres. Across the species measured from the sector files the ratio sits
// near this, so it is one calibration constant rather than a per-species fudge.
constexpr float c_UnitsPerSize = 85.0f;

// Where the lowest child sits on its parent, and how much of the parent a child
// may span. Neither is in the definition; both come from the billboards the game
// ships in its composite atlas, which show a bare lower trunk and a crown about
// half as wide as the tree is tall.
constexpr float c_FirstChild = 0.35f;
constexpr float c_LongestChild = 0.45f;

// Which curve is which, within a level. The order is the order the ids appear
// in: 6000-6007 then 6017.
enum Curve
{
    Curvature = 0,
    Weight = 1,
    Radius = 2,
    Spread = 3,
    Length = 4,
    Taper = 5,
    Density = 6,
    Angle = 7,
    Extra = 8,
};

// Per-level numbers, in the order 6008-6014.
enum Number
{
    Slices = 0,
    Segments = 1,
    Number6010 = 2,
    Number6011 = 3,
    ChildCount = 4,
    Number6013 = 5,
    Number6014 = 6,
};

struct Rng
{
    std::uint32_t state = 1;

    std::uint32_t next()
    {
        // xorshift32: cheap, repeatable, and repeatability is the point - a tree
        // has to come out the same every time it is grown from the same seed.
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    float unit() { return float(next() & 0xFFFFFF) / float(0x1000000); }
    float between(float low, float high) { return low + (high - low) * unit(); }
    float vary(float value, float variance) { return value + between(-variance, variance); }
};

struct Vec
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

Vec operator+(const Vec &a, const Vec &b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec operator-(const Vec &a, const Vec &b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec operator*(const Vec &a, float s) { return {a.x * s, a.y * s, a.z * s}; }

float dot(const Vec &a, const Vec &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec cross(const Vec &a, const Vec &b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vec normalise(const Vec &v)
{
    const float length = std::sqrt(dot(v, v));
    return length > 1e-6f ? v * (1.0f / length) : Vec{0.0f, 1.0f, 0.0f};
}

// A frame that travels along a branch. Keeping the right vector from the parent
// stops the tube twisting when the direction turns.
struct Frame
{
    Vec at;
    Vec forward{0.0f, 1.0f, 0.0f};
    Vec right{1.0f, 0.0f, 0.0f};

    Vec up() const { return cross(forward, right); }

    void reframe()
    {
        forward = normalise(forward);
        right = normalise(right - forward * dot(right, forward));
    }
};

void addVertex(MeshElement &element, const Vec &position, const Vec &normal, float u, float v)
{
    element.positions.push_back({position.x, position.y, position.z});
    element.normals.push_back({normal.x, normal.y, normal.z});
    element.texCoords.push_back({u, v});
}

// One ring of a tube, and the quads joining it to the ring before.
void addRing(MeshElement &element, const Frame &frame, float radius, float v, std::uint32_t slices, bool first)
{
    const std::uint32_t start = std::uint32_t(element.positions.size());
    const Vec up = frame.up();

    for (std::uint32_t slice = 0; slice <= slices; ++slice)
    {
        const float angle = 6.2831853f * float(slice) / float(slices);
        const Vec outward = frame.right * std::cos(angle) + up * std::sin(angle);
        addVertex(element, frame.at + outward * radius, outward, float(slice) / float(slices), v);
    }

    if (first)
        return;

    const std::uint32_t previous = start - (slices + 1);
    for (std::uint32_t slice = 0; slice < slices; ++slice)
    {
        const std::uint32_t a = previous + slice;
        const std::uint32_t b = previous + slice + 1;
        const std::uint32_t c = start + slice;
        const std::uint32_t d = start + slice + 1;
        element.indices.insert(element.indices.end(), {a, c, b, b, c, d});
    }
}

// A leaf is two quads crossed at right angles, which is what the textures in
// the archive are drawn for: they read as foliage from any direction.
void addLeafCard(MeshElement &element, const Vec &at, const Vec &facing, float width, float height,
                 const std::array<float, 8> &corners, Rng &rng)
{
    const Vec up = normalise(Vec{rng.between(-0.2f, 0.2f), 1.0f, rng.between(-0.2f, 0.2f)});
    Vec side = cross(up, normalise(facing));
    if (dot(side, side) < 1e-6f)
        side = Vec{1.0f, 0.0f, 0.0f};
    side = normalise(side);
    const Vec other = normalise(cross(up, side));

    for (int card = 0; card < 2; ++card)
    {
        const Vec across = card == 0 ? side : other;
        const Vec normal = card == 0 ? other : side;
        const std::uint32_t start = std::uint32_t(element.positions.size());

        addVertex(element, at - across * (width * 0.5f), normal, corners[2], corners[3]);
        addVertex(element, at + across * (width * 0.5f), normal, corners[0], corners[1]);
        addVertex(element, at + across * (width * 0.5f) + up * height, normal, corners[6], corners[7]);
        addVertex(element, at - across * (width * 0.5f) + up * height, normal, corners[4], corners[5]);

        element.indices.insert(element.indices.end(),
                               {start, start + 1, start + 2, start, start + 2, start + 3});
    }
}

// A frond is drawn as flat blades running along a branch rather than a tube:
// one quad per segment, each mapped to the whole tile, and the blades crossed
// about the branch axis so the spray reads from any direction. Conifer needles
// and palm leaves are this, not leaf cards.
void addFrondSegment(MeshElement &element, const Frame &from, const Frame &to, float width,
                     std::uint32_t blades, const std::array<float, 8> &corners)
{
    for (std::uint32_t blade = 0; blade < blades; ++blade)
    {
        const float turn = 3.14159265f * float(blade) / float(std::max(1u, blades));
        const Vec acrossFrom = from.right * std::cos(turn) + from.up() * std::sin(turn);
        const Vec acrossTo = to.right * std::cos(turn) + to.up() * std::sin(turn);
        const Vec normal = normalise(cross(acrossFrom, to.at - from.at));

        const std::uint32_t start = std::uint32_t(element.positions.size());
        addVertex(element, from.at - acrossFrom * (width * 0.5f), normal, corners[2], corners[3]);
        addVertex(element, from.at + acrossFrom * (width * 0.5f), normal, corners[0], corners[1]);
        addVertex(element, to.at + acrossTo * (width * 0.5f), normal, corners[6], corners[7]);
        addVertex(element, to.at - acrossTo * (width * 0.5f), normal, corners[4], corners[5]);

        element.indices.insert(element.indices.end(),
                               {start, start + 1, start + 2, start, start + 2, start + 3});
    }
}

struct Grower
{
    const SpeedTree &definition;
    const TreeGrowth &growth;
    MeshElement &bark;
    MeshElement &foliage;
    Rng rng;
    std::uint32_t branches = 0;
    std::uint32_t leaves = 0;

    // The level that carries no children is the leaf level, not a branch level:
    // in every shipping definition it reads eight slices, one segment and a
    // child count of zero.
    std::size_t branchLevels() const
    {
        return definition.levels.empty() ? 0 : definition.levels.size() - 1;
    }

    // Zero when the definition has no frond tile to draw one with.
    std::uint32_t frondLevel() const
    {
        return definition.frond.hasCorners ? definition.frond.level : 0;
    }

    float curve(std::size_t level, Curve which, float t) const
    {
        return definition.levels[level].profiles[which].at(t);
    }

    float number(std::size_t level, Number which) const { return definition.levels[level].numbers[which]; }

    void grow(std::size_t level, Frame frame, float length, float radius)
    {
        if (level >= branchLevels() || length < 1.0f || branches >= growth.branchLimit)
            return;
        ++branches;

        const auto slices = std::uint32_t(std::max(3.0f, number(level, Slices)));
        const auto segments = std::uint32_t(std::max(1.0f, number(level, Segments)));

        // Walk the branch, laying down a ring at each step and remembering where
        // each step was so children can be hung off it afterwards.
        std::vector<Frame> along;
        std::vector<float> radii;
        along.reserve(segments + 1);

        // The last branch level of a conifer or a palm is a needle spray, and a
        // tube there draws the bare sticks the first fir came out as.
        const bool asFrond = frondLevel() != 0 && level + 1 == branchLevels();
        const float frondWidth = std::max(20.0f, definition.frond.width * c_UnitsPerSize * 2.0f);

        const float lean = curve(level, Curvature, 0.0f) - curve(level, Curvature, 1.0f);
        for (std::uint32_t step = 0; step <= segments; ++step)
        {
            const float t = float(step) / float(segments);
            const float ringRadius = std::max(0.5f, radius * (1.0f - curve(level, Taper, t)) * (1.0f - t * 0.85f));

            if (!asFrond)
                addRing(bark, frame, ringRadius, t * 4.0f, slices, step == 0);
            else if (step != 0)
                addFrondSegment(foliage, along.back(), frame, frondWidth, definition.frond.blades,
                                definition.frond.hasCorners ? definition.frond.corners
                                                            : std::array<float, 8>{1, 1, 0, 1, 0, 0, 1, 0});
            along.push_back(frame);
            radii.push_back(ringRadius);

            // Bend towards the curvature the profile asks for, then step on.
            const Vec bend = frame.right * (lean * 0.02f * rng.between(-1.0f, 1.0f));
            frame.forward = normalise(frame.forward + bend);
            frame.reframe();
            frame.at = frame.at + frame.forward * (length / float(segments));
        }

        const std::size_t child = level + 1;
        if (child >= branchLevels())
        {
            hangLeaves(level, along, length);
            return;
        }

        const auto count = std::uint32_t(std::max(0.0f, number(level, ChildCount)));
        for (std::uint32_t index = 0; index < count && branches < growth.branchLimit; ++index)
        {
            // Children are spaced along the parent, not bunched at its foot: the
            // density profile decides how many survive at each height, it does
            // not decide where they sit. Reading it the other way round grows a
            // sea urchin - every branch leaving one point - which is exactly
            // what the first attempt drew.
            const float even = float(index + 1) / float(count + 1);
            const float t = c_FirstChild + (0.97f - c_FirstChild) * even;
            // Between the rings, not on them: snapping children to the rings
            // that happen to exist puts a fir's branches in flat whorls, which
            // is a artefact of the tube resolution rather than the definition.
            const float exact = t * float(segments);
            const std::size_t step = std::min(std::size_t(exact), along.size() - 1);
            const std::size_t next = std::min(step + 1, along.size() - 1);
            const float blend = exact - float(step);

            Frame base = along[step];
            base.at = along[step].at + (along[next].at - along[step].at) * blend;
            const float around = 6.2831853f * (float(index) * 0.618f + rng.unit() * 0.1f);
            const Vec outward = base.right * std::cos(around) + base.up() * std::sin(around);

            const float angle = curve(child, Angle, t) * 3.14159265f / 180.0f;
            Frame limb;
            limb.at = base.at + outward * radii[std::min(step, radii.size() - 1)];
            limb.forward = normalise(base.forward * std::cos(angle) + outward * std::sin(angle));
            limb.right = normalise(cross(limb.forward, base.forward + Vec{0.01f, 0.0f, 0.0f}));
            limb.reframe();

            // The length profile is read at the height the child attaches, which
            // is what makes a crown: mid-trunk limbs are the long ones.
            const float childLength = length * std::clamp(curve(child, Length, t), 0.05f, c_LongestChild);
            grow(child, limb, childLength, radius * std::max(0.05f, curve(child, Radius, 0.0f) * 3.0f));
        }
    }

    void hangLeaves(std::size_t level, const std::vector<Frame> &along, float length)
    {
        if (definition.leaves.empty() || along.empty())
            return;

        const LeafKind &leaf = definition.leaves.front();

        // Id 4006 is the card size in the same units as the tree size: 2.4 for
        // an oak cluster, 8.4 for a palm frond, 1.8 for a fir.
        const float width = std::max(1.0f, leaf.size[0] * c_UnitsPerSize);
        const float height = std::max(1.0f, leaf.size[1] * c_UnitsPerSize);

        // The count on the last branch level is what the definition budgets for
        // foliage on this twig.
        // A whole tree is budgeted a few hundred cards; spread over the twigs
        // that is a handful each.
        const std::array<float, 8> corners = leaf.hasCorners ? leaf.corners
                                                             : std::array<float, 8>{1, 1, 0, 1, 0, 0, 1, 0};
        const auto asked = std::uint32_t(std::clamp(number(level, ChildCount) * 0.01f, 1.0f, 4.0f));
        for (std::uint32_t index = 0; index < asked && leaves < growth.leafLimit; ++index)
        {
            // The definition says how likely a site is to carry a leaf at all,
            // which is what separates an umbrella thorn from an oak.
            if (rng.unit() > definition.leafProbability)
                continue;

            const float t = rng.between(0.3f, 1.0f);
            const Frame &frame = along[std::min(std::size_t(t * float(along.size() - 1)), along.size() - 1)];
            const Vec jitter{rng.between(-width, width), rng.between(-width, width), rng.between(-width, width)};
            addLeafCard(foliage, frame.at + jitter * 0.4f, frame.forward, width, height, corners, rng);
            ++leaves;
        }
    }
};

void boundsOf(MeshElement &element)
{
    if (element.positions.empty())
        return;
    element.boundsMin = element.boundsMax = element.positions.front();
    for (const std::array<float, 3> &position : element.positions)
        for (int axis = 0; axis < 3; ++axis)
        {
            element.boundsMin[axis] = std::min(element.boundsMin[axis], position[axis]);
            element.boundsMax[axis] = std::max(element.boundsMax[axis], position[axis]);
        }
}

} // namespace

bool growTree(const SpeedTree &definition, std::uint32_t seed, const TreeGrowth &growth, Mesh &out)
{
    out = Mesh{};
    if (definition.levels.size() < 2)
        return false;

    out.elements.resize(2);
    MeshElement &bark = out.elements[0];
    MeshElement &foliage = out.elements[1];
    bark.materialName = definition.barkTexture;
    // The leaf texture a definition names does not exist in the archives; the
    // leaves are tiles of the composite atlas the definition also names.
    foliage.materialName = definition.billboardTexture;

    Grower grower{definition, growth, bark, foliage, Rng{seed ? seed : 1u}, 0, 0};

    // Size varies per instance around what the definition asks for, which is how
    // one file grows a wood rather than a row of identical trees.
    const float wanted = growth.size > 0.0f ? growth.size : definition.size;
    // The variance is as wide as the size itself for the winter bushes, so an
    // unlucky draw asks for a tree of nothing. The game's own trees vary by a
    // factor of about two and a half, never to zero, so the draw has a floor.
    const float size = growth.applyVariance
                           ? std::max(wanted * 0.4f, grower.rng.vary(wanted, definition.sizeVariance))
                           : wanted;

    Frame trunk;
    grower.grow(0, trunk, size * c_UnitsPerSize, size * c_UnitsPerSize * 0.05f);

    boundsOf(bark);
    boundsOf(foliage);

    // The curves give the shape; the definition's own recorded extent gives the
    // scale. Growing from size alone put the palms at two and a half times the
    // height the game places them at and the acacias at two fifths, because a
    // palm and an acacia of the same size are nothing alike. Scaling the finished
    // tree to the recorded height fixes the proportion without touching the
    // silhouette, and the variance still applies through the size.
    if (definition.recordedHeight > 1.0f)
    {
        float grownHeight = bark.boundsMax[1] - bark.boundsMin[1];
        if (!foliage.positions.empty())
            grownHeight = std::max(grownHeight, foliage.boundsMax[1] - foliage.boundsMin[1]);

        if (grownHeight > 1.0f)
        {
            const float wantedHeight = definition.recordedHeight * (size / std::max(1.0f, definition.size));
            const float scale = wantedHeight / grownHeight;
            for (MeshElement *element : {&bark, &foliage})
            {
                for (std::array<float, 3> &position : element->positions)
                    for (int axis = 0; axis < 3; ++axis)
                        position[axis] *= scale;
                boundsOf(*element);
            }
        }
    }

    out.boundsMin = bark.boundsMin;
    out.boundsMax = bark.boundsMax;
    for (int axis = 0; axis < 3; ++axis)
    {
        if (!foliage.positions.empty())
        {
            out.boundsMin[axis] = std::min(out.boundsMin[axis], foliage.boundsMin[axis]);
            out.boundsMax[axis] = std::max(out.boundsMax[axis], foliage.boundsMax[axis]);
        }
    }
    return true;
}

} // namespace genome
