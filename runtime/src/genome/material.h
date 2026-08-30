#pragma once

// Compiled material (.xshmat): an eCResourceShaderMaterial_PS whose only real
// content is one nested property set whose CLASS NAME is the effect - there is
// no shader id field, the class is the shader. Its declared properties carry the
// render state, and the textures live one level further down.
//
// Below the effect sits a small DAG of colour-source nodes. Each of the shader's
// fixed slots (Diffuse, Normal, Specular, ...) is a proxy holding a GUID, and
// that GUID is matched against the `token` of one node in a flat element list -
// there is no index, only a search. A node is a sampler (a texture), a constant,
// or a combiner/blend that mixes two or three further proxies.
//
// We collapse that DAG the way a fixed-function material model needs: walk down
// from each slot, follow src1 through combiners and blends, and take the first
// sampler found. That is what g3dit's own renderer does and it is lossy - a
// Multiply of two textures becomes just the first one. What is NOT lossy is
// `samplers`, which lists every texture the graph mentions, so a renderer that
// later grows real graph support has the references it needs.
//
// The texture name a sampler stores is a SOURCE asset path (a .tga that never
// shipped) and it may name a variant that is not the one on disk; resolveTexture
// below turns it into a file that exists.

#include "property_set.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace genome
{

// The nested set's class name. Anything outside this list is still parsed as far
// as its property block, but its slot order is unknown so no textures come out.
enum class ShaderKind : std::uint32_t
{
    Unknown,
    Default,
    Skin,
    Leaf,
    Water,
    Particle,
};

const char *shaderKindName(ShaderKind kind);

// eEShaderMaterialBlendMode. An engine enum, not a blend equation: Normal is
// opaque, Masked is opaque plus an alpha discard, and only the middle three are
// actually blended.
enum class BlendMode : std::uint32_t
{
    Normal = 0,
    Masked = 1,
    AlphaBlend = 2,
    Modulate = 3,
    AlphaModulate = 4,
    Translucent = 5,
    Darken = 6,
    Brighten = 7,
    Invisible = 8,
};

const char *blendModeName(BlendMode mode);

// eEShaderMaterialTransformation. This is what says which vertex path a mesh
// wants - Skinned is the one that matters for characters.
enum class Transformation : std::uint32_t
{
    Default = 0,
    Instanced = 1,
    Skinned = 2,
    TreeBranches = 3,
    TreeFronds = 4,
    TreeLeafs = 5,
    Billboard = 6,
};

const char *transformationName(Transformation transformation);

// eEColorSrcSampleTexRepeat, one per axis; maps 1:1 onto a sampler address mode.
enum class TexRepeat : std::uint32_t
{
    Wrap = 0,
    Clamp = 1,
    Mirror = 2,
};

const char *texRepeatName(TexRepeat repeat);

// eEColorSrcSwitchRepeat: how a variant index wraps when a body part asks for a
// higher switch than the texture set has members. Per sampler, not per material.
enum class SwitchRepeat : std::uint32_t
{
    Repeat = 0,
    Clamp = 1,
    PingPong = 2,
};

const char *switchRepeatName(SwitchRepeat repeat);

// The union of every effect's slots. Each class serialises only its own, in its
// own order, which is why shaderSlots() exists rather than a fixed layout.
enum class Slot : std::uint32_t
{
    Diffuse,
    Opacity,
    SelfIllumination,
    Specular,
    SpecularPower,
    Normal,
    Distortion,
    SubSurface,
    StaticBump,
    FlowingBump,
    Reflection,
    Count,
};

constexpr std::size_t c_SlotCount = static_cast<std::size_t>(Slot::Count);

const char *slotName(Slot slot);

// The proxy order a shader class writes, which is also the only thing that tells
// a reader which proxy is which. Empty for an effect we do not know.
const std::vector<Slot> &shaderSlots(ShaderKind kind);

struct Sampler
{
    std::string imagePath;   // ImageFilePath as authored, e.g. "G3_Orc_Warrior_Body_Diffuse_S1.tga"
    std::string baseName;    // directory and extension stripped, lower-cased
    SwitchRepeat switchRepeat = SwitchRepeat::Repeat;
    TexRepeat repeatU = TexRepeat::Wrap;
    TexRepeat repeatV = TexRepeat::Wrap;
    float animationSpeed = 0.0f;
    std::uint32_t texCoordSet = 0;

    // True when the name takes part in the variant rule below, i.e. it ends in
    // "_s1". The compiler writes variant sets that way and authors only ever
    // reference member one.
    bool switchable() const;
};

struct SlotBinding
{
    bool bound = false;        // the slot's proxy GUID is valid, so something is wired in
    bool sampled = false;      // and walking down from it reached a texture
    bool throughGraph = false; // via a combiner or blend rather than a sampler directly
    int sampler = -1;          // index into Material::samplers, or -1
    std::string sourceClass;   // class of the node the slot points straight at
};

struct Material
{
    std::string name;          // resource name; only legacy files carry one
    ShaderKind kind = ShaderKind::Unknown;
    std::string shaderClass;   // the class name verbatim, including unknown ones
    std::uint16_t shaderVersion = 0;
    std::uint16_t materialVersion = 0;

    BlendMode blendMode = BlendMode::Normal;
    std::uint8_t maskReference = 0;
    Transformation transformation = Transformation::Default;
    bool enableSpecular = false;
    bool disableLighting = false;
    bool useDepthBias = false;
    std::string fallbackMaterial;

    // Declared on the resource itself rather than on the effect: these are
    // collision attributes, and they are the only properties a .xshmat carries
    // that say nothing about how it looks.
    std::uint32_t physicsMaterial = 0; // eEShapeMaterial; the enum is not in the SDK headers
    bool ignoredByTraceRay = false;
    bool disableCollision = false;
    bool disableResponse = false;

    std::array<SlotBinding, c_SlotCount> slots;
    std::vector<Sampler> samplers; // every sampler in the graph, in element order

    std::size_t nodeCount = 0;    // colour-source nodes in the graph
    std::size_t opaqueNodes = 0;  // of those, classes we skip rather than decode

    // Bytes between the end of the graph and the shader's declared end. Legacy
    // editor materials park about a kilobyte of undecoded proxy list there;
    // zero means the walk consumed the effect byte for byte.
    std::size_t undecodedTail = 0;

    const SlotBinding &binding(Slot slot) const { return slots[static_cast<std::size_t>(slot)]; }
    const Sampler *texture(Slot slot) const;

    bool alphaTested() const { return blendMode == BlendMode::Masked; }
    bool blended() const;
    bool invisible() const { return blendMode == BlendMode::Invisible; }
    bool skinned() const { return transformation == Transformation::Skinned; }

    // g3dit's heuristic, not engine truth: the file stores a reference byte and
    // nothing says how the engine turned it into a discard threshold.
    float alphaTestThreshold() const;

    // INFERRED, not read. Nothing in a .xshmat stores cull mode, depth write or
    // a two-sided flag. Foliage and alpha-tested cut-outs are the cases that
    // visibly break when culled, so those are what we guess at.
    bool twoSided() const;
};

// Parses a whole .xshmat file. Returns false and fills `error` on malformed data.
bool loadMaterial(const std::vector<std::uint8_t> &bytes, Material &material, std::string *error = nullptr);

// Asked whether a candidate exists in _compiledImage.pak. The argument is a bare
// lower-case basename without extension; the caller decides how to look it up.
using ImageExists = std::function<bool(const std::string &)>;

struct TextureResolution
{
    std::string fileName;    // "g3_orc_warrior_body_diffuse_s3.ximg", or empty if nothing ships
    bool switched = false;   // the variant rule picked a member other than the authored one
    bool renamed = false;    // the trailing "_NN" -> "_s1" fallback fired
    int variantCount = 0;    // how many consecutive _s1.._sN exist; 0 when not a variant set
    int variantIndex = 0;    // 0-based member chosen
};

// Turns a sampler's authored source path into a file present in the image
// archive. `materialSwitch` is the body part's skin variant and comes from the
// VISUAL, not from the material - a material is shared by every orc and the
// switch is what makes one green and the next grey. It is 0-based, and 0 means
// "use exactly what the author wrote".
TextureResolution resolveTexture(const Sampler &sampler, int materialSwitch, const ImageExists &exists);

} // namespace genome
