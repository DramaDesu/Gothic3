#include "spt.h"

#include "reader.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace genome
{
namespace
{

constexpr std::uint32_t c_MagicId = 1000;
constexpr char c_Magic[] = "__IdvSpt_02_";

// Widths in bytes, or one of these for the two variable forms.
constexpr int c_String = -1;
constexpr int c_Array32 = -2;   // a u32 count, then that many 32-byte records

// Recovered by requiring that one table parse all 98 shipping files with no
// bytes left over. An empty string and a zero-length array read exactly like a
// plain word, so where both fit, the longer form is the true one - a file that
// carries a non-empty value settles it.
const std::unordered_map<std::uint32_t, int> &widthTable()
{
    static const std::unordered_map<std::uint32_t, int> table = {
        {1000, c_String}, {1002, 0}, {1003, 4}, {1005, 4}, {1006, 4}, {1007, 0}, {1008, 4}, {1009, 0},
        {1012, 4}, {1014, 4}, {1016, 0}, {1017, 4},
        {2000, c_String}, {2001, 4}, {2002, 1}, {2003, 4}, {2005, 4}, {2006, 4}, {2007, 4},
        {3000, 4}, {3001, 4}, {3002, 4}, {3003, 1}, {3004, 4}, {3005, 4}, {3006, 1}, {3007, 4},
        {3008, 4}, {3009, 1}, {3010, 4},
        {4000, 1}, {4001, 12}, {4002, 4}, {4003, c_String}, {4004, 12}, {4005, 12}, {4006, 12}, {4007, 4},
        {5000, 12}, {5001, 12}, {5002, 12}, {5003, 12}, {5004, 12}, {5005, 4}, {5006, 1},
        {6000, c_String}, {6001, c_String}, {6002, c_String}, {6003, c_String}, {6004, c_String},
        {6005, c_String}, {6006, c_String}, {6007, c_String}, {6008, 4}, {6009, 4}, {6010, 4},
        {6011, 4}, {6012, 4}, {6013, 4}, {6014, 4}, {6015, 1}, {6016, 1}, {6017, c_String},
        {8000, 0}, {8001, 4}, {8002, 4}, {8003, 52}, {8004, 4}, {8005, 52}, {8006, 4}, {8007, 4},
        {8008, 4}, {8009, 52},
        {9002, 4}, {9003, 4}, {9004, 4}, {9005, 0}, {9006, 4}, {9007, 4}, {9008, 4}, {9009, 4},
        {9010, 4}, {9011, 4}, {9012, 4}, {9013, 4}, {9014, 4},
        {10001, 4}, {10002, c_Array32}, {10003, c_Array32}, {10004, c_Array32},
        {11000, 0}, {11001, 4}, {11002, 4},
        {12001, 4}, {12002, 16}, {12003, 20}, {12004, 24},
        {13002, 4}, {13003, 4}, {13004, 4}, {13005, c_String}, {13006, 4}, {13007, 1}, {13008, 4},
        {13009, 4}, {13010, 4}, {13011, 4}, {13012, 4}, {13013, 4},
        {14000, 0}, {14001, 4}, {14002, c_String}, {14003, 4}, {14004, 4}, {14005, 4}, {14006, 4},
        {14007, 4}, {14008, 4},
        {15000, 0}, {15001, 4}, {15002, 1}, {15003, 4},
        {16001, 0}, {16002, 4}, {16003, 4}, {16004, 4}, {16005, 4}, {16006, 4}, {16007, 4},
        {16008, 4}, {16009, 4}, {16010, 4}, {16011, 4}, {16012, 4}, {16013, 4}, {16014, 4},
        {18000, 0}, {18001, 4}, {18002, 12}, {18003, 12}, {18004, 12}, {18005, c_String},
        {19001, 4}, {19002, 4},
        {20002, c_String}, {20003, 1}, {20004, 1}, {20005, 52},
        {22000, 1},
    };
    return table;
}

// The spline ids in the order they appear in a level, which is the order
// BranchLevel::profiles stores them in.
constexpr std::uint32_t c_SplineIds[9] = {6000, 6001, 6002, 6003, 6004, 6005, 6006, 6007, 6017};

std::string readString(Reader &reader)
{
    const std::uint32_t length = reader.u32();
    if (!reader.ok() || length > reader.remaining())
    {
        reader.fail();
        return {};
    }
    std::string text(reinterpret_cast<const char *>(reader.data()) + reader.tell(), length);
    reader.skip(length);
    return text;
}

// The text form is
//     BezierSpline <low> <high> <variance>
//     {
//         <count>
//         <t> <value> <tangentX> <tangentY> <tangentLength>   x count
//     }
bool parseSpline(const std::string &text, Spline &out)
{
    const char *at = text.c_str();
    const char *end = at + text.size();

    const auto word = [&]() {
        while (at < end && (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r'))
            ++at;
        const char *start = at;
        while (at < end && *at != ' ' && *at != '\t' && *at != '\n' && *at != '\r')
            ++at;
        return std::string(start, at);
    };

    if (word() != "BezierSpline")
        return false;
    out.low = std::strtof(word().c_str(), nullptr);
    out.high = std::strtof(word().c_str(), nullptr);
    out.variance = std::strtof(word().c_str(), nullptr);
    if (word() != "{")
        return false;

    const long count = std::strtol(word().c_str(), nullptr, 10);
    if (count < 0 || count > 4096)
        return false;

    out.points.clear();
    out.points.reserve(std::size_t(count));
    for (long index = 0; index < count; ++index)
    {
        Spline::Point point;
        point.t = std::strtof(word().c_str(), nullptr);
        point.value = std::strtof(word().c_str(), nullptr);
        point.tangentX = std::strtof(word().c_str(), nullptr);
        point.tangentY = std::strtof(word().c_str(), nullptr);
        point.tangentLength = std::strtof(word().c_str(), nullptr);
        out.points.push_back(point);
    }
    return word() == "}";
}

} // namespace

float Spline::shape(float t) const
{
    if (points.empty())
        return 0.0f;
    if (points.size() == 1 || t <= points.front().t)
        return points.front().value;
    if (t >= points.back().t)
        return points.back().value;

    std::size_t index = 0;
    while (index + 2 < points.size() && points[index + 1].t < t)
        ++index;

    const Point &a = points[index];
    const Point &b = points[index + 1];
    const float span = b.t - a.t;
    if (span <= 0.0f)
        return a.value;

    // Each point carries a unit tangent and a length, which is a cubic Bezier
    // written as a Hermite curve: the control handles sit along those tangents.
    const float u = (t - a.t) / span;
    const float aHandle = a.tangentX > 0.0f ? a.tangentY / a.tangentX * a.tangentLength : 0.0f;
    const float bHandle = b.tangentX > 0.0f ? b.tangentY / b.tangentX * b.tangentLength : 0.0f;

    const float u2 = u * u;
    const float u3 = u2 * u;
    const float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
    const float h10 = u3 - 2.0f * u2 + u;
    const float h01 = -2.0f * u3 + 3.0f * u2;
    const float h11 = u3 - u2;
    return h00 * a.value + h10 * aHandle + h01 * b.value + h11 * bHandle;
}

float Spline::at(float t) const
{
    return low + (high - low) * shape(t);
}

bool loadSpeedTree(const std::vector<std::uint8_t> &bytes, SpeedTree &out, std::string *error)
{
    const auto fail = [&](const char *reason) {
        if (error)
            *error = reason;
        return false;
    };

    out = SpeedTree{};
    Reader reader(bytes);

    if (reader.u32() != c_MagicId)
        return fail("not a .spt file");
    if (readString(reader) != c_Magic)
        return fail("unexpected .spt version");

    const auto &widths = widthTable();
    BranchLevel level;
    bool levelStarted = false;
    LeafKind leaf;
    bool leafStarted = false;

    while (reader.ok() && reader.remaining() > 0)
    {
        const std::size_t offset = reader.tell();
        const std::uint32_t id = reader.u32();
        if (!reader.ok())
            return fail("truncated .spt");

        const auto known = widths.find(id);
        if (known == widths.end())
            return fail("unknown .spt token");

        SpeedTreeToken token;
        token.id = id;
        token.offset = offset;

        const int width = known->second;
        if (width == c_String)
        {
            token.text = readString(reader);
        }
        else if (width == c_Array32)
        {
            const std::uint32_t count = reader.u32();
            if (!reader.ok() || count > reader.remaining() / 32)
                return fail("bad .spt array");
            token.floats.resize(std::size_t(count) * 8);
            for (float &value : token.floats)
                value = reader.f32();
        }
        else if (width == 1)
        {
            token.floats.push_back(float(reader.u8()));
        }
        else if (width > 0)
        {
            if (width % 4 != 0)
                return fail("bad .spt width");
            token.floats.resize(std::size_t(width) / 4);
            for (float &value : token.floats)
                value = reader.f32();
        }

        if (!reader.ok())
            return fail("truncated .spt token");

        // A level ends when its first spline comes round again.
        if (id == c_SplineIds[0])
        {
            if (levelStarted)
                out.levels.push_back(level);
            level = BranchLevel{};
            levelStarted = true;
        }
        if (id == 4003)
        {
            if (leafStarted)
                out.leaves.push_back(leaf);
            leaf = LeafKind{};
            leafStarted = true;
            leaf.texture = token.text;
        }

        const auto asFloats3 = [&token]() {
            std::array<float, 3> value{};
            for (std::size_t index = 0; index < 3 && index < token.floats.size(); ++index)
                value[index] = token.floats[index];
            return value;
        };

        switch (id)
        {
        case 2000:
            out.barkTexture = token.text;
            break;
        case 2001:
            out.parameter2001 = token.floats.empty() ? 0.0f : token.floats[0];
            break;
        case 2003:
            out.parameter2003 = token.floats.empty() ? 0.0f : token.floats[0];
            break;
        case 2005:
            // A seed, so the integer reading is the meaningful one.
            std::memcpy(&out.seed, &token.floats[0], sizeof(out.seed));
            break;
        case 2006:
            out.size = token.floats[0];
            break;
        case 2007:
            out.sizeVariance = token.floats[0];
            break;
        case 4004:
            leaf.pivot = asFloats3();
            break;
        case 4005:
            leaf.variance = asFloats3();
            break;
        case 4006:
            leaf.size = asFloats3();
            break;
        case 4002:
            leaf.scale = token.floats.empty() ? 0.0f : token.floats[0];
            break;
        case 18005:
            out.shadowTexture = token.text;
            break;
        case 20002:
            out.billboardTexture = token.text;
            break;
        case 8003:
        case 8005:
        case 8009:
        case 20005: {
            SpeedTreeMaterial material;
            const float *value = token.floats.data();
            material.ambient = {value[0], value[1], value[2]};
            material.diffuse = {value[3], value[4], value[5]};
            material.specular = {value[6], value[7], value[8]};
            material.emissive = {value[9], value[10], value[11]};
            material.shininess = value[12];
            out.materials.push_back(material);
            break;
        }
        default:
            break;
        }

        if (levelStarted)
        {
            for (std::size_t slot = 0; slot < 9; ++slot)
            {
                if (id != c_SplineIds[slot])
                    continue;
                if (!parseSpline(token.text, level.profiles[slot]))
                    return fail("bad spline in .spt");
            }
            if (id >= 6008 && id <= 6014)
            {
                float value = token.floats.empty() ? 0.0f : token.floats[0];
                // 6008 and 6009 are counts, so the four bytes are an integer.
                // Read as a float a count of 8 becomes 1.1e-44, which rounds to
                // nothing and grows a one-segment, three-sided spike instead of
                // a trunk - which is exactly what the first attempt drew.
                if (id == 6008 || id == 6009)
                {
                    std::uint32_t raw = 0;
                    std::memcpy(&raw, &value, sizeof(raw));
                    value = float(raw);
                }
                level.numbers[id - 6008] = value;
            }
            if (id == 6015 || id == 6016)
                level.flags[id - 6015] = std::uint8_t(token.floats.empty() ? 0.0f : token.floats[0]);
        }

        out.tokens.push_back(std::move(token));
    }

    if (levelStarted)
        out.levels.push_back(level);
    if (leafStarted)
        out.leaves.push_back(leaf);

    // Ids 10002 to 10004 carry one quad each - four (u, v) corners of the tile a
    // leaf kind takes from the composite atlas. They come after the kinds, in
    // the same order, and a kind with no quad is one the atlas does not serve.
    std::size_t kind = 0;
    for (const SpeedTreeToken &token : out.tokens)
    {
        if (token.id < 10002 || token.id > 10004 || token.floats.size() < 8)
            continue;
        if (kind >= out.leaves.size())
            break;
        for (std::size_t corner = 0; corner < 8; ++corner)
            out.leaves[kind].corners[corner] = token.floats[corner];
        out.leaves[kind].hasCorners = true;
        ++kind;
    }

    return reader.ok() ? true : fail("truncated .spt");
}

} // namespace genome
