#include "material.h"

#include <algorithm>
#include <cctype>

namespace genome
{
namespace
{

// A bCGuid is 16 raw bytes plus a validity word. When the low byte of that word
// is clear the 16 bytes are uninitialised heap memory, so equality has to demand
// validity on both sides or unset proxies would all "match" each other.
struct Guid
{
    std::array<std::uint8_t, 16> bytes{};
    bool valid = false;

    bool operator==(const Guid &other) const { return valid && other.valid && bytes == other.bytes; }
};

struct Proxy
{
    std::uint32_t component = 0; // eEShaderColorSrcComponent, an undocumented channel selector
    Guid guid;
};

// One node of the colour-source graph, already reduced to what we use: its class,
// the token slots are matched against, and its inputs.
struct GraphNode
{
    std::string className;
    Guid token;
    int sampler = -1;          // index into Material::samplers when this node is one
    std::vector<Proxy> inputs; // combiner and blend sources, src1 first
};

// Bytes an eCShaderEllementBase tail occupies at the end of every node body:
// u16 version, bCGuid token, bCRect editor layout.
constexpr std::size_t c_ElementTailSize = 2 + 20 + 16;

Guid readGuid(Reader &reader)
{
    Guid guid;
    reader.array(guid.bytes.data(), guid.bytes.size());
    guid.valid = (reader.u32() & 0xFFu) != 0;
    return guid;
}

Proxy readProxy(Reader &reader)
{
    Proxy proxy;
    proxy.component = reader.u32();
    proxy.guid = readGuid(reader);
    return proxy;
}

std::string lowered(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return text;
}

// A string property holds an entry, and in a wrapped file an entry is a two-byte
// index into the string table rather than the characters themselves. Property
// cannot know that on its own, so the table has to decode the value here.
std::string propertyString(const Property &property, const StringTable &strings)
{
    Reader reader(property.value.data(), property.value.size());
    const std::string value = strings.entry(reader);
    return reader.ok() ? value : std::string();
}

// bTPropertyContainer<enum X> is six bytes: a version word that is always 1,
// then the value.
bool propertyEnum(const Property *property, std::uint32_t &value)
{
    if (property == nullptr || property->value.size() != 6)
        return false;
    std::memcpy(&value, property->value.data() + 2, sizeof(value));
    return true;
}

bool propertyBool(const Property *property, bool fallback)
{
    if (property == nullptr || property->value.size() != 1)
        return fallback;
    return property->value[0] != 0;
}

std::uint8_t propertyByte(const Property *property, std::uint8_t fallback)
{
    if (property == nullptr || property->value.size() != 1)
        return fallback;
    return property->value[0];
}

template <typename T> T propertyAsEnum(const Property *property, T fallback, std::uint32_t highest)
{
    std::uint32_t value = 0;
    if (!propertyEnum(property, value) || value > highest)
        return fallback;
    return static_cast<T>(value);
}

// Strips any directory and the extension, then lower-cases. Both slash flavours
// occur in authored paths, and 1089 of the shipping references still carry a
// directory that means nothing to the compiled archive.
std::string sourceBaseName(const std::string &path)
{
    const std::size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos)
        name.erase(dot);
    return lowered(std::move(name));
}

ShaderKind classifyShader(const std::string &className)
{
    if (className == "eCShaderDefault")
        return ShaderKind::Default;
    if (className == "eCShaderSkin")
        return ShaderKind::Skin;
    if (className == "eCShaderLeaf")
        return ShaderKind::Leaf;
    if (className == "eCShaderWater")
        return ShaderKind::Water;
    if (className == "eCShaderParticle")
        return ShaderKind::Particle;
    return ShaderKind::Unknown;
}

// Reads one node of the graph. The class-specific fields come first, then the
// two base versions and the token; classes whose fields nobody has reverse
// engineered are skipped whole, which is safe because the header declares where
// the node ends.
bool readGraphNode(Reader &reader, const StringTable &strings, Material &material, GraphNode &node)
{
    PropertySetHeader header;
    if (!readPropertySetHeader(reader, strings, header))
        return false;
    node.className = header.className;

    bool decoded = true;
    if (header.className == "eCColorSrcSampler")
    {
        Sampler sampler;
        sampler.texCoordSet = reader.u32(); // eCTexCoordSrcProxy: the vertex UV set
        readGuid(reader);                   // and the tex-coord node feeding it, which we ignore
        reader.u32();                       // eEColorSrcSamplerType: undocumented, 0xFFFFFFFF two thirds of the time

        if (const Property *path = header.find("ImageFilePath"))
        {
            sampler.imagePath = propertyString(*path, strings);
            sampler.baseName = sourceBaseName(sampler.imagePath);
        }
        sampler.switchRepeat = propertyAsEnum(header.find("SwitchRepeat"), SwitchRepeat::Repeat, 2);
        sampler.repeatU = propertyAsEnum(header.find("TexRepeatU"), TexRepeat::Wrap, 2);
        sampler.repeatV = propertyAsEnum(header.find("TexRepeatV"), TexRepeat::Wrap, 2);
        if (const Property *speed = header.find("AnimationSpeed"))
            sampler.animationSpeed = speed->asFloat().value_or(0.0f);

        node.sampler = static_cast<int>(material.samplers.size());
        material.samplers.push_back(std::move(sampler));
    }
    else if (header.className == "eCColorSrcCombiner")
    {
        node.inputs.push_back(readProxy(reader));
        node.inputs.push_back(readProxy(reader));
    }
    else if (header.className == "eCColorSrcBlend")
    {
        node.inputs.push_back(readProxy(reader));
        node.inputs.push_back(readProxy(reader));
        node.inputs.push_back(readProxy(reader));
    }
    else if (header.className != "eCColorSrcConstant" && header.className != "eCColorSrcVertexColor" &&
             header.className != "eCColorSrcCubeSampler")
    {
        // eCTexCoordSrc* and eCColorSrcSkydomeSampler: no parser anywhere decodes
        // their fields, and they never carry a texture, so skipping them costs
        // nothing as long as the token still comes out.
        decoded = false;
        ++material.opaqueNodes;
    }

    if (decoded)
    {
        reader.skip(2); // eCColorSrcBase version, always 1
        reader.skip(2); // eCShaderEllementBase version, always 1
        node.token = readGuid(reader);
        reader.skip(16); // editor layout rectangle
    }

    // Whether we walked the body or skipped it, the token sits at a fixed
    // distance from the declared end, so an undecoded node still gets one - and
    // a decoded node that did not land exactly on the end was misread, so take
    // the tail there too rather than trusting the walk.
    if (!decoded || reader.tell() != header.declaredEnd)
    {
        if (header.declaredEnd < c_ElementTailSize)
            return false;
        reader.seek(header.declaredEnd - c_ElementTailSize);
        reader.skip(2);
        node.token = readGuid(reader);
    }

    reader.seek(header.declaredEnd);
    return reader.ok();
}

// Follows a slot's proxy down to the first sampler. Combiners and blends mix
// several sources and we keep only src1, which is what g3dit's renderer does;
// the full set is still in Material::samplers for anyone who wants it.
void bindSlot(const std::vector<GraphNode> &nodes, const Proxy &proxy, SlotBinding &binding)
{
    binding.bound = proxy.guid.valid;
    if (!binding.bound)
        return;

    Guid wanted = proxy.guid;
    for (std::size_t depth = 0; depth < 16; ++depth)
    {
        const auto match = std::find_if(nodes.begin(), nodes.end(),
                                        [&](const GraphNode &node) { return node.token == wanted; });
        if (match == nodes.end())
            return;
        if (depth == 0)
            binding.sourceClass = match->className;
        else
            binding.throughGraph = true;

        if (match->sampler >= 0)
        {
            binding.sampled = true;
            binding.sampler = match->sampler;
            return;
        }
        if (match->inputs.empty() || !match->inputs.front().guid.valid)
            return;
        wanted = match->inputs.front().guid;
    }
}

// Turns "..._diffuse_01" into "..._diffuse_s1". The material compiler renamed
// numbered textures into single-member switched sets, so an authored reference
// to the numbered original no longer matches anything on disk.
std::string renamedVariant(const std::string &baseName)
{
    std::size_t digits = baseName.size();
    while (digits > 0 && std::isdigit(static_cast<unsigned char>(baseName[digits - 1])) != 0)
        --digits;
    if (digits == baseName.size() || digits == 0 || baseName[digits - 1] != '_')
        return {};
    return baseName.substr(0, digits) + "s1";
}

} // namespace

const char *shaderKindName(ShaderKind kind)
{
    switch (kind)
    {
        case ShaderKind::Default: return "eCShaderDefault";
        case ShaderKind::Skin: return "eCShaderSkin";
        case ShaderKind::Leaf: return "eCShaderLeaf";
        case ShaderKind::Water: return "eCShaderWater";
        case ShaderKind::Particle: return "eCShaderParticle";
        default: return "unknown";
    }
}

const char *blendModeName(BlendMode mode)
{
    switch (mode)
    {
        case BlendMode::Normal: return "Normal";
        case BlendMode::Masked: return "Masked";
        case BlendMode::AlphaBlend: return "AlphaBlend";
        case BlendMode::Modulate: return "Modulate";
        case BlendMode::AlphaModulate: return "AlphaModulate";
        case BlendMode::Translucent: return "Translucent";
        case BlendMode::Darken: return "Darken";
        case BlendMode::Brighten: return "Brighten";
        case BlendMode::Invisible: return "Invisible";
        default: return "unknown";
    }
}

const char *transformationName(Transformation transformation)
{
    switch (transformation)
    {
        case Transformation::Default: return "Default";
        case Transformation::Instanced: return "Instanced";
        case Transformation::Skinned: return "Skinned";
        case Transformation::TreeBranches: return "Tree_Branches";
        case Transformation::TreeFronds: return "Tree_Fronds";
        case Transformation::TreeLeafs: return "Tree_Leafs";
        case Transformation::Billboard: return "Billboard";
        default: return "unknown";
    }
}

const char *texRepeatName(TexRepeat repeat)
{
    switch (repeat)
    {
        case TexRepeat::Wrap: return "Wrap";
        case TexRepeat::Clamp: return "Clamp";
        case TexRepeat::Mirror: return "Mirror";
        default: return "unknown";
    }
}

const char *switchRepeatName(SwitchRepeat repeat)
{
    switch (repeat)
    {
        case SwitchRepeat::Repeat: return "Repeat";
        case SwitchRepeat::Clamp: return "Clamp";
        case SwitchRepeat::PingPong: return "PingPong";
        default: return "unknown";
    }
}

const char *slotName(Slot slot)
{
    switch (slot)
    {
        case Slot::Diffuse: return "Diffuse";
        case Slot::Opacity: return "Opacity";
        case Slot::SelfIllumination: return "SelfIllumination";
        case Slot::Specular: return "Specular";
        case Slot::SpecularPower: return "SpecularPower";
        case Slot::Normal: return "Normal";
        case Slot::Distortion: return "Distortion";
        case Slot::SubSurface: return "SubSurface";
        case Slot::StaticBump: return "StaticBump";
        case Slot::FlowingBump: return "FlowingBump";
        case Slot::Reflection: return "Reflection";
        default: return "unknown";
    }
}

const std::vector<Slot> &shaderSlots(ShaderKind kind)
{
    static const std::vector<Slot> none;
    static const std::vector<Slot> defaults{Slot::Diffuse,  Slot::Opacity,       Slot::SelfIllumination,
                                            Slot::Specular, Slot::SpecularPower, Slot::Normal};
    static const std::vector<Slot> skin{Slot::Diffuse,      Slot::Opacity, Slot::SelfIllumination, Slot::Specular,
                                        Slot::SpecularPower, Slot::Normal, Slot::SubSurface};
    static const std::vector<Slot> leaf{Slot::Diffuse, Slot::Specular, Slot::SpecularPower, Slot::Normal};
    static const std::vector<Slot> water{Slot::Diffuse,  Slot::StaticBump,    Slot::FlowingBump,
                                         Slot::Specular, Slot::SpecularPower, Slot::Reflection};
    static const std::vector<Slot> particle{Slot::Diffuse};

    switch (kind)
    {
        case ShaderKind::Default: return defaults;
        case ShaderKind::Skin: return skin;
        case ShaderKind::Leaf: return leaf;
        case ShaderKind::Water: return water;
        case ShaderKind::Particle: return particle;
        default: return none;
    }
}

bool Sampler::switchable() const
{
    return baseName.size() > 3 && baseName.compare(baseName.size() - 3, 3, "_s1") == 0;
}

const Sampler *Material::texture(Slot slot) const
{
    const SlotBinding &bound = binding(slot);
    if (!bound.sampled || bound.sampler < 0 || static_cast<std::size_t>(bound.sampler) >= samplers.size())
        return nullptr;
    return &samplers[static_cast<std::size_t>(bound.sampler)];
}

bool Material::blended() const
{
    switch (blendMode)
    {
        case BlendMode::AlphaBlend:
        case BlendMode::AlphaModulate:
        case BlendMode::Translucent:
        case BlendMode::Modulate:
        case BlendMode::Darken:
        case BlendMode::Brighten: return true;
        default: return false;
    }
}

float Material::alphaTestThreshold() const
{
    return 0.95f - static_cast<float>(maskReference) / 255.0f;
}

bool Material::twoSided() const
{
    return kind == ShaderKind::Leaf || blendMode == BlendMode::Masked;
}

bool loadMaterial(const std::vector<std::uint8_t> &bytes, Material &material, std::string *error)
{
    const auto fail = [&](const char *reason) {
        if (error)
            *error = reason;
        return false;
    };

    material = Material();

    Reader reader(bytes);
    const StringTable strings = StringTable::sniff(reader);
    if (!reader.ok())
        return fail("bad file wrapper");

    PropertySetHeader header;
    if (!readPropertySetHeader(reader, strings, header))
        return fail("bad property set header");
    if (header.className != "eCResourceShaderMaterial_PS")
        return fail("not a material");

    material.name = header.objectName;
    material.materialVersion = header.classVersion;
    propertyEnum(header.find("PhysicMaterial"), material.physicsMaterial);
    material.ignoredByTraceRay = propertyBool(header.find("IgnoredByTraceRay"), false);
    material.disableCollision = propertyBool(header.find("DisableCollision"), false);
    material.disableResponse = propertyBool(header.find("DisableResponse"), false);

    std::uint16_t resourceVersion = 0;
    if (!readResourceBase(reader, resourceVersion))
        return fail("bad resource header");

    // The effect is a nested property set and its class name is the only thing
    // that names the shader.
    PropertySetHeader shader;
    if (!readPropertySetHeader(reader, strings, shader))
        return fail("bad shader header");

    material.shaderClass = shader.className;
    material.kind = classifyShader(shader.className);
    material.shaderVersion = shader.classVersion;

    material.blendMode = propertyAsEnum(shader.find("BlendMode"), BlendMode::Normal, 8);
    material.transformation = propertyAsEnum(shader.find("TransformationType"), Transformation::Default, 6);
    material.maskReference = propertyByte(shader.find("MaskReference"), 0);
    material.enableSpecular = propertyBool(shader.find("EnableSpecular"), false);
    material.disableLighting = propertyBool(shader.find("DisableLighting"), false);
    material.useDepthBias = propertyBool(shader.find("UseDepthBias"), false);
    if (const Property *fallback = shader.find("FallbackMaterial"))
        material.fallbackMaterial = propertyString(*fallback, strings);

    const std::vector<Slot> &slots = shaderSlots(material.kind);
    if (slots.empty())
    {
        // An effect whose slot order we do not know: the render state above is
        // still good, but reading past it would be guesswork, so stop here and
        // let the caller see an empty slot set rather than invented textures.
        if (!reader.ok() || reader.tell() > shader.declaredEnd)
            return fail("unknown shader class");
        material.undecodedTail = shader.declaredEnd - reader.tell();
        reader.seek(shader.declaredEnd);
        return reader.ok() && reader.tell() == header.declaredEnd ? true : fail("unknown shader class");
    }

    // One proxy per slot, in the class's own order. Distortion is version-gated
    // and comes last on the three classes that have it.
    std::vector<Proxy> proxies;
    proxies.reserve(slots.size() + 1);
    for (std::size_t index = 0; index < slots.size(); ++index)
        proxies.push_back(readProxy(reader));

    const bool hasDistortion = material.kind == ShaderKind::Default || material.kind == ShaderKind::Water ||
                               material.kind == ShaderKind::Particle;
    std::vector<Slot> order = slots;
    if (hasDistortion && shader.classVersion > 1)
    {
        proxies.push_back(readProxy(reader));
        order.push_back(Slot::Distortion);
    }
    if (!reader.ok())
        return fail("bad shader slots");

    reader.skip(2);  // eCShaderBase version, always 1
    reader.skip(2);  // eCShaderEllementBase version, always 1
    reader.skip(20); // the shader's own token
    reader.skip(16); // editor layout rectangle

    const std::uint32_t nodeCount = reader.u32();
    if (!reader.ok() || nodeCount > 4096)
        return fail("implausible graph size");

    std::vector<GraphNode> nodes(nodeCount);
    for (std::uint32_t index = 0; index < nodeCount; ++index)
    {
        if (!readGraphNode(reader, strings, material, nodes[index]))
            return fail("bad colour source node");
    }
    material.nodeCount = nodeCount;

    for (std::size_t index = 0; index < order.size(); ++index)
        bindSlot(nodes, proxies[index], material.slots[static_cast<std::size_t>(order[index])]);

    // Legacy editor materials carry about a kilobyte of undecoded proxy list
    // after the graph. Everything a renderer needs was read before it, so trust
    // the declared end rather than the walk - but the material's own end still
    // has to agree, or we misparsed something that matters.
    if (!reader.ok() || reader.tell() > shader.declaredEnd)
        return fail("graph overran the shader");
    material.undecodedTail = shader.declaredEnd - reader.tell();
    reader.seek(shader.declaredEnd);
    if (reader.tell() != header.declaredEnd)
        return fail("body did not end where the header said it would");

    return reader.ok();
}

TextureResolution resolveTexture(const Sampler &sampler, int materialSwitch, const ImageExists &exists)
{
    TextureResolution result;
    if (sampler.baseName.empty() || !exists)
        return result;

    // Two spellings are worth trying: what the author wrote, and the compiler's
    // rename of a numbered texture into member one of a switched set.
    std::string candidates[2] = {sampler.baseName, renamedVariant(sampler.baseName)};

    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const std::string &name = candidates[attempt];
        if (name.empty())
            continue;

        const bool switchable = name.size() > 3 && name.compare(name.size() - 3, 3, "_s1") == 0;
        if (switchable && materialSwitch != 0)
        {
            // Count the set by asking the archive, not by trusting the name. A
            // normal map usually ships a single member even where the diffuse
            // has ten, so substituting the index blindly would miss the file.
            const std::string base = name.substr(0, name.size() - 1);
            int count = 0;
            while (count < 256 && exists(base + std::to_string(count + 1)))
                ++count;
            if (count == 0)
                continue;

            int index = 0;
            switch (sampler.switchRepeat)
            {
                case SwitchRepeat::Clamp: index = std::clamp(materialSwitch, 0, count - 1); break;
                case SwitchRepeat::PingPong:
                    index = materialSwitch % count;
                    if ((index & 1) != 0)
                        index = count - index - 1;
                    break;
                default: index = materialSwitch % count; break;
            }

            result.fileName = base + std::to_string(index + 1) + ".ximg";
            result.switched = index != 0;
            result.renamed = attempt == 1;
            result.variantCount = count;
            result.variantIndex = index;
            return result;
        }

        if (exists(name))
        {
            result.fileName = name + ".ximg";
            result.renamed = attempt == 1;
            return result;
        }
    }

    return result;
}

} // namespace genome
