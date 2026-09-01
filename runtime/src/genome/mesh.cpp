#include "mesh.h"

namespace genome
{
namespace
{

// Element size per stream type. The table cycles in blocks above 12, which is
// how the engine encodes further UV sets and tangent-space streams.
std::size_t elementSize(std::uint32_t streamType)
{
    switch (streamType)
    {
        case 0: return 4;   // index
        case 1: return 12;  // position
        case 2: return 16;  // transformed position
        case 3: return 12;  // normal
        case 4: return 4;   // diffuse
        case 5: return 4;   // specular
        case 6: return 4;   // point size
        default: break;
    }
    if (streamType >= 7 && streamType <= 11)
        return 4; // blend weights, never present in a static mesh
    if (streamType == 12)
        return 8;
    if (streamType >= 13 && streamType <= 59)
    {
        switch ((streamType - 13) % 3)
        {
            case 0: return 12;
            case 1: return 16;
            default: return 8;
        }
    }
    if (streamType >= 60 && streamType <= 63)
        return 16;
    if (streamType >= 64 && streamType <= 67)
        return 12;
    if (streamType >= 68 && streamType <= 71)
        return 8;
    if (streamType == 72)
        return 12;
    if (streamType == 73)
        return 8;
    return 0;
}

template <typename T> void readInto(Reader &reader, std::vector<T> &destination, std::uint32_t count)
{
    destination.resize(count);
    reader.array(destination.data(), count);
}

bool readMeshElement(Reader &reader, const StringTable &strings, MeshElement &element)
{
    const std::uint16_t version = reader.u16();
    reader.u32(); // FVF, reconstructible from the streams themselves
    reader.array(element.boundsMin.data(), 3);
    reader.array(element.boundsMax.data(), 3);
    reader.u32(); // memory-footprint hint, not a byte count: never seek with it
    element.materialName = strings.entry(reader);

    const std::uint32_t streamCount = reader.u32();
    if (!reader.ok() || streamCount > 64)
        return false;

    for (std::uint32_t index = 0; index < streamCount && reader.ok(); ++index)
    {
        const std::uint32_t streamType = reader.u32();
        reader.skip(2); // array version, always 1
        reader.skip(1); // container's leading byte, always 1
        const std::uint32_t count = reader.u32();

        const std::size_t stride = elementSize(streamType);
        if (stride == 0 || !reader.ok() || count > reader.remaining() / stride)
            return false;

        element.streams.push_back(streamType);

        // Streams appear in no fixed order, so dispatch on the type.
        switch (static_cast<StreamType>(streamType))
        {
        case StreamType::Face: readInto(reader, element.indices, count); break;
        case StreamType::Position: readInto(reader, element.positions, count); break;
        case StreamType::Normal: readInto(reader, element.normals, count); break;
        case StreamType::Tangent: readInto(reader, element.tangents, count); break;
        case StreamType::TexCoord0: readInto(reader, element.texCoords, count); break;
        case StreamType::Diffuse: readInto(reader, element.diffuse, count); break;
        case StreamType::Specular: readInto(reader, element.vertexLight, count); break;
        default: reader.skip(count * stride); break;
        }
    }

    // Trailing per-element blocks: lightmap groups and a spatial hierarchy. We
    // skip them, but they have to be parsed exactly or the next element starts
    // at the wrong offset.
    if (version >= 3)
    {
        for (int block = 0; block < 2; ++block)
        {
            reader.skip(1);
            const std::uint32_t count = reader.u32();
            reader.skip(static_cast<std::size_t>(count) * 4);
        }
    }
    if (version >= 2)
    {
        reader.skip(1);
        const std::uint32_t groupCount = reader.u32();
        for (std::uint32_t group = 0; group < groupCount && reader.ok(); ++group)
        {
            reader.skip(1);
            const std::uint32_t first = reader.u32();
            reader.skip(static_cast<std::size_t>(first) * 4);
            reader.skip(1);
            const std::uint32_t second = reader.u32();
            reader.skip(static_cast<std::size_t>(second) * 4);
            reader.skip(12 + 64 + 8); // vector, matrix, vector2
        }
    }
    if (version >= 4)
    {
        const std::uint32_t hierarchyCount = reader.u32();
        reader.skip(static_cast<std::size_t>(hierarchyCount) * 24);
        reader.skip(1);
        const std::uint32_t remapCount = reader.u32();
        reader.skip(static_cast<std::size_t>(remapCount) * 4);
    }

    return reader.ok();
}

} // namespace

std::size_t Mesh::vertexCount() const
{
    std::size_t total = 0;
    for (const MeshElement &element : elements)
        total += element.positions.size();
    return total;
}

std::size_t Mesh::triangleCount() const
{
    std::size_t total = 0;
    for (const MeshElement &element : elements)
        total += element.triangleCount();
    return total;
}

bool loadMesh(const std::vector<std::uint8_t> &bytes, Mesh &mesh, std::string *error)
{
    const auto fail = [&](const char *reason) {
        if (error)
            *error = reason;
        return false;
    };

    Reader reader(bytes);
    const StringTable strings = StringTable::sniff(reader);
    if (!reader.ok())
        return fail("bad file wrapper");

    PropertySetHeader header;
    if (!readPropertySetHeader(reader, strings, header))
        return fail("bad property set header");
    if (header.className != "eCResourceMeshComplex_PS")
        return fail("not a mesh");

    mesh.name = header.objectName;
    if (const Property *bounds = header.find("BoundingBox"))
        bounds->asBox(mesh.boundsMin.data(), mesh.boundsMax.data());

    std::uint16_t resourceVersion = 0;
    if (!readResourceBase(reader, resourceVersion))
        return fail("bad resource header");

    reader.f32(); // resource priority, zero throughout shipping data
    const std::uint32_t elementCount = reader.u32();
    if (!reader.ok() || elementCount > 256)
        return fail("implausible element count");

    mesh.elements.clear();
    mesh.elements.resize(elementCount);
    for (std::uint32_t index = 0; index < elementCount; ++index)
    {
        if (!readMeshElement(reader, strings, mesh.elements[index]))
            return fail("bad mesh element");
    }

    // The header declares where the body ends; disagreeing means we misparsed.
    if (reader.tell() != header.declaredEnd)
        return fail("body did not end where the header said it would");

    return true;
}

} // namespace genome
