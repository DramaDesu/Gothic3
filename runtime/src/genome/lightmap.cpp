#include "lightmap.h"

#include "property_set.h"
#include "reader.h"

namespace genome
{
namespace
{

// bTValArray: a one-byte array version, a count, then the entries raw.
template <typename T> bool readArray(Reader &reader, std::vector<T> &out, std::size_t componentsPerEntry)
{
    reader.skip(1);
    const std::uint32_t count = reader.u32();
    if (!reader.ok() || count > reader.remaining() / (componentsPerEntry * sizeof(T)))
        return false;

    out.resize(std::size_t(count) * componentsPerEntry);
    reader.array(out.data(), out.size());
    return reader.ok();
}

// eCLightmap: the lighting of one mesh element.
bool readElement(Reader &reader, LightmapElement &out)
{
    const std::uint16_t version = reader.u16();
    if (!reader.ok() || version < 2)
        return false;

    if (!readArray(reader, out.colours, 1))
        return false;

    // A direction per vertex, three floats. Reading these at four floats apiece
    // is what made an earlier attempt see 150 KB of meaningless 16-byte records:
    // the vector (1, 0, 0) stepped at the wrong stride reads as (0, 1, 0, 0).
    if (version > 2 && !readArray(reader, out.incident, 3))
        return false;

    // Then the bitmaps. Each one opens with a marker - which is what an earlier
    // reading mistook for the end of the record, and why it stopped after the
    // first element and left 150 KB unread.
    const std::uint16_t bitmapCount = reader.u16();
    if (!reader.ok() || bitmapCount > 4096)
        return false;

    for (std::uint16_t index = 0; index < bitmapCount && reader.ok(); ++index)
    {
        if (reader.u32() != 0x11223344)
            return false;

        LightmapBitmap bitmap;
        reader.skip(2); // record version
        bitmap.uvSet = std::int32_t(reader.u32());
        bitmap.offsetX = std::int32_t(reader.u32());
        bitmap.offsetY = std::int32_t(reader.u32());
        bitmap.width = std::int32_t(reader.u32());
        bitmap.height = std::int32_t(reader.u32());

        const std::uint32_t size = reader.u32();
        if (!reader.ok() || size > reader.remaining())
            return false;
        bitmap.data.resize(size);
        reader.array(bitmap.data.data(), size);

        // A second block follows a non-empty one; nothing reads it yet.
        if (size > 0)
            reader.skip(reader.u32());

        out.bitmaps.push_back(std::move(bitmap));
    }

    return reader.ok();
}

} // namespace

std::size_t Lightmap::vertexCount() const
{
    std::size_t total = 0;
    for (const LightmapElement &element : elements)
        total += element.vertexCount();
    return total;
}

bool loadLightmap(const std::vector<std::uint8_t> &bytes, Lightmap &out, std::string *error)
{
    const auto fail = [&](const char *reason) {
        if (error)
            *error = reason;
        return false;
    };

    out = Lightmap{};
    Reader reader(bytes);

    const StringTable strings = StringTable::sniff(reader);
    if (!strings.wrapped())
        return fail("not a wrapped genome file");

    PropertySetHeader header;
    if (!readPropertySetHeader(reader, strings, header))
        return fail("bad property set header");
    if (header.className != "eCResourceLightmap_PS")
        return fail("not a lightmap");

    // The class version is the header's last field, so it is already read - the
    // engine calls it the lightmap version and refuses anything below 4.
    const std::uint16_t version = header.classVersion;
    if (version < 4)
        return fail("unsupported lightmap version");

    std::uint16_t resourceVersion = 0;
    if (!readResourceBase(reader, resourceVersion))
        return fail("bad resource header");

    out.resourcePriority = reader.f32();
    out.meshName = strings.entry(reader);
    // The engine stops here when the mesh is unnamed, and so do we.
    if (out.meshName.empty())
        return reader.ok();

    out.type = static_cast<LightmapType>(reader.u32());
    if (version > 1)
        out.scaling = reader.f32();

    const std::uint32_t count = reader.u32();
    if (!reader.ok() || count > 4096)
        return fail("implausible element count");

    // One per mesh element, each optional.
    for (std::uint32_t index = 0; index < count && reader.ok(); ++index)
    {
        LightmapElement element;
        if (reader.u8() != 0 && !readElement(reader, element))
            return fail("bad element");
        out.elements.push_back(std::move(element));
    }

    // A grid of ambient occlusion may follow the elements; nothing reads it yet.
    return reader.ok() ? true : fail("truncated lightmap");
}

} // namespace genome
