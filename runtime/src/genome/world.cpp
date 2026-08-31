#include "world.h"

namespace genome
{
namespace
{

constexpr std::uint16_t c_ArchiveVersion = 83;
constexpr std::size_t c_EntityBodySize = 298;
constexpr std::uint32_t c_RecordMarker = 0xDEADC0DE;

// Offsets inside the entity body. The file order of the bounding volumes is not
// the member order in the engine headers, so these are taken from the data.
constexpr std::size_t c_NameOffset = 41;
constexpr std::size_t c_WorldMatrixOffset = 43;
constexpr std::size_t c_WorldBoundsOffset = 219;
constexpr std::size_t c_PropertySetCountOffset = 294;

// A property set inside an entity is written in a slightly different shape from
// a standalone resource, so it is read here rather than through
// readPropertySetHeader.
bool readEntityPropertySet(Reader &reader, const StringTable &strings, std::string &className,
                           std::vector<Property> &properties)
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

    // The class body past the properties varies per class; the declared end is
    // what makes it skippable without knowing any of them. Each record is then
    // closed by a marker, and forgetting it lands the next read inside the tail
    // of this one.
    reader.seek(declaredEnd);
    if (reader.u32() != c_RecordMarker)
        return false;
    return reader.ok();
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

        reader.seek(bodyStart + c_NameOffset);
        placement.name = strings.entry(reader);

        reader.seek(bodyStart + c_WorldMatrixOffset);
        reader.array(placement.world.data(), 16);

        reader.seek(bodyStart + c_WorldBoundsOffset);
        reader.array(placement.boundsMin.data(), 3);
        reader.array(placement.boundsMax.data(), 3);

        reader.seek(bodyStart + c_PropertySetCountOffset);
        const std::uint32_t propertySetCount = reader.u32();
        if (!reader.ok() || propertySetCount > 256)
            return fail("implausible property set count");

        std::vector<Property> properties;
        for (std::uint32_t set = 0; set < propertySetCount; ++set)
        {
            std::string className;
            if (!readEntityPropertySet(reader, strings, className, properties))
                return fail("bad entity property set");

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
