#include "property_set.h"

#include <algorithm>

namespace genome
{
namespace
{

constexpr char c_WrapperMagic[8] = {'G', 'E', 'N', 'O', 'M', 'F', 'L', 'E'};
constexpr std::uint32_t c_StringTableMarker = 0xDEADBEEF;
constexpr std::uint16_t c_VersionWithoutObjectName = 81;
constexpr std::uint16_t c_VersionWithoutGuid = 82;

} // namespace

std::optional<float> Property::asFloat() const
{
    if (value.size() != sizeof(float))
        return std::nullopt;
    float result = 0.0f;
    std::memcpy(&result, value.data(), sizeof(result));
    return result;
}

std::optional<std::string> Property::asString() const
{
    // A string property holds one entry, which in a wrapped file was already
    // resolved when the property was read.
    if (value.empty())
        return std::string();
    return std::string(reinterpret_cast<const char *>(value.data()), value.size());
}

bool Property::asBox(float outMin[3], float outMax[3]) const
{
    if (value.size() != 24)
        return false;
    std::memcpy(outMin, value.data(), 12);
    std::memcpy(outMax, value.data() + 12, 12);
    return true;
}

StringTable StringTable::sniff(Reader &reader)
{
    StringTable table;
    table.m_payloadEnd = reader.size();

    if (!reader.match(c_WrapperMagic, sizeof(c_WrapperMagic)))
        return table;

    reader.skip(sizeof(c_WrapperMagic));
    const std::uint16_t version = reader.u16();
    const std::uint32_t tableOffset = reader.u32();
    if (version != 1 || tableOffset >= reader.size())
    {
        reader.fail();
        return table;
    }

    const std::size_t payload = reader.tell();
    reader.seek(tableOffset);
    if (reader.u32() != c_StringTableMarker)
    {
        reader.fail();
        return table;
    }

    if (reader.u8() != 0)
    {
        const std::uint32_t count = reader.u32();
        table.m_strings.reserve(count);
        for (std::uint32_t index = 0; index < count && reader.ok(); ++index)
            table.m_strings.push_back(reader.string16());
    }

    table.m_wrapped = true;
    table.m_payloadEnd = tableOffset;
    reader.seek(payload);
    return table;
}

std::string StringTable::entry(Reader &reader) const
{
    if (!m_wrapped)
        return reader.string16();

    const std::uint16_t index = reader.u16();
    if (index >= m_strings.size())
    {
        reader.fail();
        return {};
    }
    return m_strings[index];
}

const Property *PropertySetHeader::find(std::string_view name) const
{
    const auto match = std::find_if(properties.begin(), properties.end(),
                                    [&](const Property &property) { return property.name == name; });
    return match == properties.end() ? nullptr : &*match;
}

bool readPropertySetHeader(Reader &reader, const StringTable &strings, PropertySetHeader &header)
{
    // Six bytes of class marker that never vary in shipping data.
    reader.skip(6);
    header.className = strings.entry(reader);
    reader.skip(1);
    reader.skip(2);
    header.version = reader.u16();
    reader.skip(2); // the version is written twice

    const std::uint32_t bodySize = reader.u32();
    header.declaredEnd = reader.tell() + bodySize;

    if (header.version < c_VersionWithoutObjectName)
        header.objectName = strings.entry(reader);
    if (header.version < c_VersionWithoutGuid)
        reader.skip(20); // 16-byte guid plus a validity word

    reader.skip(2); // property block version, always 30
    const std::uint32_t propertyCount = reader.u32();
    if (!reader.ok() || propertyCount > 4096)
        return false;

    header.properties.clear();
    header.properties.reserve(propertyCount);
    for (std::uint32_t index = 0; index < propertyCount && reader.ok(); ++index)
    {
        Property property;
        property.name = strings.entry(reader);
        property.type = strings.entry(reader);
        reader.skip(2); // per-property version
        const std::uint32_t valueSize = reader.u32();
        if (!reader.ok() || valueSize > reader.remaining())
            return false;
        property.value.resize(valueSize);
        reader.array(property.value.data(), valueSize);
        header.properties.push_back(std::move(property));
    }

    header.classVersion = reader.u16();
    return reader.ok();
}

bool readResourceBase(Reader &reader, std::uint16_t &resourceVersion)
{
    resourceVersion = reader.u16();
    if (resourceVersion >= 0x17)
        reader.u32(); // engine-side size hint, not a file offset
    if (resourceVersion < 0x1E)
    {
        const std::uint16_t tag = reader.u16();
        if (tag > 1)
            reader.skip(1);
    }
    return reader.ok();
}

} // namespace genome
