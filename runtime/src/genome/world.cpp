#include "world.h"

#include <cstdio>

#include <cmath>
#include <unordered_map>

namespace genome
{
namespace
{

constexpr std::uint16_t c_ArchiveVersion = 83;
constexpr std::size_t c_EntityBodySize = 298;
constexpr std::uint32_t c_RecordMarker = 0xDEADC0DE;

// Offsets inside the entity body. The file order of the bounding volumes is not
// the member order in the engine headers, so these are taken from the data.
constexpr std::size_t c_GuidOffset = 4;
constexpr std::size_t c_NameOffset = 41;
constexpr std::size_t c_WorldMatrixOffset = 43;
constexpr std::size_t c_WorldBoundsOffset = 219;
constexpr std::size_t c_LodFactorOffset = 275;
constexpr std::size_t c_CullFactorOffset = 280;
constexpr std::size_t c_PropertySetCountOffset = 294;

// A property set inside an entity is written in a slightly different shape from
// a standalone resource, so it is read here rather than through
// readPropertySetHeader.
bool readEntityPropertySet(Reader &reader, const StringTable &strings, std::string &className,
                           std::vector<Property> &properties, std::size_t *bodyStart = nullptr,
                           std::size_t *bodyEnd = nullptr)
{
    reader.skip(2); // property-set version, signed and sometimes -1
    reader.skip(6); // subclass identifier
    className = strings.entry(reader);
    reader.skip(5); // filler, last two bytes repeat the version below
    const std::uint16_t version = reader.u16();
    const std::uint32_t bodySize = reader.u32();
    const std::size_t declaredEnd = reader.tell() + bodySize;
    if (!reader.ok() || declaredEnd > reader.size())
        return false;

    if (version == 1)
        strings.entry(reader); // object name
    if (version <= 0x51)
        reader.skip(20); // guid

    reader.skip(2); // property block version
    const std::uint32_t propertyCount = reader.u32();
    if (!reader.ok() || propertyCount > 4096)
        return false;

    if (bodyEnd)
        *bodyEnd = declaredEnd;

    properties.clear();
    properties.reserve(propertyCount);
    for (std::uint32_t index = 0; index < propertyCount && reader.ok(); ++index)
    {
        Property property;
        property.name = strings.entry(reader);
        property.type = strings.entry(reader);
        reader.skip(2);
        const std::uint32_t valueSize = reader.u32();
        if (!reader.ok() || valueSize > reader.remaining())
            return false;
        property.value.resize(valueSize);
        reader.array(property.value.data(), valueSize);
        properties.push_back(std::move(property));
    }

    if (bodyStart)
        *bodyStart = reader.tell();

    // The class body past the properties varies per class; the declared end is
    // what makes it skippable without knowing any of them. Each record is then
    // closed by a marker, and forgetting it lands the next read inside the tail
    // of this one.
    reader.seek(declaredEnd);
    if (reader.u32() != c_RecordMarker)
        return false;
    return reader.ok();
}

constexpr std::size_t c_PlantEntrySize = 44;

// A plant is stored as position, rotation and two scales - width across, height
// up - which the same row-vector convention as the entity matrices turns into
// the transform the renderer wants.
WorldMatrix plantMatrix(const std::array<float, 3> &at, const std::array<float, 4> &q, float width, float height)
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    WorldMatrix m{};
    m[0] = (1.0f - 2.0f * (y * y + z * z)) * width;
    m[1] = (2.0f * (x * y + w * z)) * width;
    m[2] = (2.0f * (x * z - w * y)) * width;
    m[4] = (2.0f * (x * y - w * z)) * height;
    m[5] = (1.0f - 2.0f * (x * x + z * z)) * height;
    m[6] = (2.0f * (y * z + w * x)) * height;
    m[8] = (2.0f * (x * z + w * y)) * width;
    m[9] = (2.0f * (y * z - w * x)) * width;
    m[10] = (1.0f - 2.0f * (x * x + y * y)) * width;
    m[12] = at[0];
    m[13] = at[1];
    m[14] = at[2];
    m[15] = 1.0f;
    return m;
}

// A rotated box is bounded by summing the absolute contribution of each axis.
void transformBounds(const std::array<float, 3> &boxMin, const std::array<float, 3> &boxMax, const WorldMatrix &m,
                     std::array<float, 3> &outMin, std::array<float, 3> &outMax)
{
    for (int axis = 0; axis < 3; ++axis)
    {
        float centre = m[12 + axis];
        float extent = 0.0f;
        for (int source = 0; source < 3; ++source)
        {
            const float c = (boxMin[source] + boxMax[source]) * 0.5f;
            const float e = (boxMax[source] - boxMin[source]) * 0.5f;
            centre += c * m[source * 4 + axis];
            extent += e * std::fabs(m[source * 4 + axis]);
        }
        outMin[axis] = centre - extent;
        outMax[axis] = centre + extent;
    }
}

// A GUID, in the order Windows stores one: three little-endian fields and then
// eight bytes as they lie. Written back out in the form the lightmap file names
// use, so the two can simply be compared.
std::string readGuid(Reader &reader)
{
    const std::uint32_t first = reader.u32();
    const std::uint16_t second = reader.u16();
    const std::uint16_t third = reader.u16();
    std::uint8_t rest[8]{};
    for (std::uint8_t &value : rest)
        value = reader.u8();
    if (!reader.ok())
        return {};

    char text[40];
    std::snprintf(text, sizeof(text), "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", first, second, third,
                  rest[0], rest[1], rest[2], rest[3], rest[4], rest[5], rest[6], rest[7]);
    return text;
}

// A nested subclass omits the leading version field a top-level record carries,
// so it starts straight at the identifier.
bool readSubClassHeader(Reader &reader, const StringTable &strings, std::string &className, std::size_t &declaredEnd)
{
    reader.skip(6);
    className = strings.entry(reader);
    reader.skip(5);
    reader.skip(2); // version
    const std::uint32_t bodySize = reader.u32();
    declaredEnd = reader.tell() + bodySize;
    return reader.ok() && declaredEnd <= reader.size();
}

template <typename T> void readPrefixedList(Reader &reader, std::vector<T> &out, std::size_t components)
{
    reader.skip(1); // list prefix
    const std::uint32_t count = reader.u32();
    if (!reader.ok() || count > reader.remaining() / (components * 4))
    {
        reader.fail();
        return;
    }
    out.resize(count);
    reader.array(reinterpret_cast<float *>(out.data()), count * components);
}

// eCVegetation_PS: a table of plant meshes, then a grid that scatters them.
// The meshes are templates - a single clump of blades in local space - and the
// grid entries place them, so a sector of grass costs 59 meshes rather than
// thousands.
int readVegetation(Reader &reader, const StringTable &strings, std::size_t declaredEnd,
                   std::vector<VegetationMesh> &meshes, std::vector<VegetationInstance> &instances)
{
    reader.skip(2); // class version, which the caller stops just short of

    if (reader.u8() != 0)
    {
        const std::uint32_t typeCount = reader.u32();
        for (std::uint32_t index = 0; index < typeCount && reader.ok(); ++index)
        {
            reader.skip(2);
            strings.entry(reader);
        }
    }

    reader.u32(); // highest mesh id, equal to the count in shipping data
    const std::uint32_t meshCount = reader.u32();
    if (!reader.ok() || meshCount > 4096)
        return 1;

    // Grid entries name a mesh by its id, which need not be its position in
    // this table, so keep the mapping while reading.
    std::unordered_map<std::uint16_t, std::uint32_t> meshOfId;

    for (std::uint32_t index = 0; index < meshCount && reader.ok(); ++index)
    {
        std::string className;
        std::size_t meshEnd = 0;
        if (!readSubClassHeader(reader, strings, className, meshEnd))
            return 2;

        if (className != "eCVegetation_Mesh")
        {
            reader.seek(meshEnd);
            continue;
        }

        // Skip the declared properties; the geometry lives in the class body.
        reader.skip(2);
        const std::uint32_t propertyCount = reader.u32();
        for (std::uint32_t property = 0; property < propertyCount && reader.ok(); ++property)
        {
            reader.skip(6);
            const std::uint32_t valueSize = reader.u32();
            reader.skip(valueSize);
        }
        reader.skip(2); // class version

        VegetationMesh plant;
        reader.u16(); // mesh type
        const std::uint16_t meshId = reader.u16();
        reader.skip(8); // timestamp
        plant.texture = strings.entry(reader);
        reader.array(plant.boundsMin.data(), 3);
        reader.array(plant.boundsMax.data(), 3);

        readPrefixedList(reader, plant.positions, 3);
        readPrefixedList(reader, plant.normals, 3);
        readPrefixedList(reader, plant.texCoords, 2);

        reader.skip(1);
        const std::uint32_t indexCount = reader.u32();
        if (!reader.ok() || indexCount > reader.remaining() / 4)
            return 3;
        plant.indices.resize(indexCount);
        reader.array(plant.indices.data(), indexCount);

        if (!plant.positions.empty() && !plant.indices.empty())
        {
            meshOfId.emplace(meshId, std::uint32_t(meshes.size()));
            meshes.push_back(std::move(plant));
        }

        reader.seek(meshEnd);
    }

    if (reader.u16() != 2) // grid version
        return 4;
    reader.f32();    // node dimension, 1000 units in shipping data
    reader.skip(16); // grid rectangle, in nodes

    const std::uint32_t nodeCount = reader.u32();
    if (!reader.ok() || nodeCount > reader.remaining() / 34)
        return 5;

    for (std::uint32_t node = 0; node < nodeCount && reader.ok(); ++node)
    {
        reader.u32(); // node index within the grid rectangle
        if (reader.u16() != 1)
            return 6;
        reader.skip(24); // node bounds, which the entry positions already imply

        const std::uint32_t entryCount = reader.u32();
        if (!reader.ok() || entryCount > reader.remaining() / c_PlantEntrySize)
            return 7;

        for (std::uint32_t entry = 0; entry < entryCount && reader.ok(); ++entry)
        {
            reader.u16(); // plant type
            const std::uint16_t meshId = reader.u16();
            std::array<float, 3> at{};
            std::array<float, 4> rotation{};
            reader.array(at.data(), 3);
            reader.array(rotation.data(), 4);
            const float scaleWidth = reader.f32();
            const float scaleHeight = reader.f32();
            reader.u32(); // tint, unused so far

            const auto found = meshOfId.find(meshId);
            if (found == meshOfId.end())
                continue;

            VegetationInstance plant;
            plant.mesh = found->second;
            plant.world = plantMatrix(at, rotation, scaleWidth, scaleHeight);
            transformBounds(meshes[plant.mesh].boundsMin, meshes[plant.mesh].boundsMax, plant.world, plant.boundsMin,
                            plant.boundsMax);
            instances.push_back(plant);
        }
    }

    reader.seek(declaredEnd);
    return reader.ok() ? 0 : 8;
}

} // namespace

std::size_t WorldLayer::meshCount() const
{
    std::size_t count = 0;
    for (const Placement &placement : placements)
        count += placement.meshName.empty() ? 0 : 1;
    return count;
}

bool loadWorldNode(const std::vector<std::uint8_t> &bytes, WorldLayer &layer, std::string *error)
{
    const auto fail = [&](const char *reason) {
        if (error)
            *error = reason;
        return false;
    };

    Reader reader(bytes);
    const StringTable strings = StringTable::sniff(reader);
    if (!reader.ok() || !strings.wrapped())
        return fail("not a wrapped genome file");

    if (reader.u16() != c_ArchiveVersion)
        return fail("unexpected archive version");

    const std::int32_t entityCount = static_cast<std::int32_t>(reader.u32());
    if (!reader.ok() || entityCount < 0 || entityCount > 1000000)
        return fail("implausible entity count");

    layer.placements.clear();
    layer.placements.reserve(static_cast<std::size_t>(entityCount));

    for (std::int32_t index = 0; index < entityCount; ++index)
    {
        // Spatial prologue. In shipping sector files no entity carries a
        // creator, so the guid and the matching epilogue never appear - but the
        // flags are read rather than assumed.
        const std::uint8_t hasCreatorOuter = reader.u8();
        const std::uint8_t disablePatch = reader.u8();
        reader.skip(2); // eCSpatialEntity version
        const std::uint8_t hasCreator = reader.u8();
        if (hasCreator)
            reader.skip(20);
        reader.skip(24); // visual world node boundary
        reader.skip(60); // oriented boundary

        const std::size_t bodyStart = reader.tell();
        if (bodyStart + c_EntityBodySize > reader.size())
            return fail("entity body runs past the end of the file");

        Placement placement;

        reader.seek(bodyStart + c_GuidOffset);
        placement.guid = readGuid(reader);

        reader.seek(bodyStart + c_NameOffset);
        placement.name = strings.entry(reader);

        reader.seek(bodyStart + c_WorldMatrixOffset);
        reader.array(placement.world.data(), 16);

        reader.seek(bodyStart + c_WorldBoundsOffset);
        reader.array(placement.boundsMin.data(), 3);
        reader.array(placement.boundsMax.data(), 3);

        reader.seek(bodyStart + c_LodFactorOffset);
        placement.visualLodFactor = reader.f32();
        reader.seek(bodyStart + c_CullFactorOffset);
        placement.objectCullFactor = reader.f32();

        reader.seek(bodyStart + c_PropertySetCountOffset);
        const std::uint32_t propertySetCount = reader.u32();
        if (!reader.ok() || propertySetCount > 256)
            return fail("implausible property set count");

        std::vector<Property> properties;
        for (std::uint32_t set = 0; set < propertySetCount; ++set)
        {
            std::string className;
            std::size_t vegetationBody = 0, vegetationEnd = 0;
            if (!readEntityPropertySet(reader, strings, className, properties, &vegetationBody, &vegetationEnd))
                return fail("bad entity property set");

            if (className == "eCVegetation_PS")
            {
                // readEntityPropertySet has already stepped past this record, so
                // rewind to its body and read the geometry, then carry on.
                const std::size_t resume = reader.tell();
                reader.seek(vegetationBody);
                const int stage = readVegetation(reader, strings, vegetationEnd, layer.vegetationMeshes,
                                                 layer.vegetation);
                if (stage != 0)
                    return fail("bad vegetation");
                reader.seek(resume);
                continue;
            }

            if (className == "eCStaticPointLight_PS")
            {
                PointLight light;
                light.position = placement.translation();
                for (const Property &property : properties)
                {
                    Reader value(property.value);
                    if (property.name == "Color" && property.value.size() >= 16)
                    {
                        // Four bytes of tag, then red, green and blue.
                        value.skip(4);
                        for (float &channel : light.colour)
                            channel = value.f32();
                    }
                    else if (property.name == "Range" && property.value.size() >= 4)
                        light.range = value.f32();
                    else if (property.name == "CastShadows" && !property.value.empty())
                        light.castShadows = property.value[0] != 0;
                    else if (property.name == "Offset" && property.value.size() >= 12)
                    {
                        for (float &axis : light.position)
                            axis += value.f32();
                    }
                }
                if (light.range > 0.0f)
                    layer.lights.push_back(light);
                continue;
            }

            if (className == "eCSpeedTree_PS")
            {
                TreePlacement tree;
                tree.name = placement.name;
                tree.world = placement.world;
                tree.boundsMin = placement.boundsMin;
                tree.boundsMax = placement.boundsMax;
                for (const Property &property : properties)
                {
                    Reader value(property.value);
                    if (property.name == "ResourceFilePath" && property.value.size() >= 2)
                        tree.resource = strings.entry(value);
                    else if (property.name == "EnableWind" && !property.value.empty())
                        tree.wind = property.value[0] != 0;
                }
                if (!tree.resource.empty())
                    layer.trees.push_back(std::move(tree));
                continue;
            }

            if (className != "eCVisualMeshStatic_PS")
                continue;
            for (const Property &property : properties)
            {
                // A string property in a wrapped file is a two-byte index, not
                // the text itself.
                if (property.name != "ResourceFileName" || property.value.size() < 2)
                    continue;
                Reader value(property.value);
                placement.meshName = strings.entry(value);
            }
        }

        if (hasCreatorOuter && !disablePatch)
            reader.skip(20); // template guid epilogue

        layer.placements.push_back(std::move(placement));
        if (!reader.ok())
            return fail("truncated entity list");
    }

    return true;
}

} // namespace genome
