# Lighting

The world had none until now: the fragment shader multiplied the diffuse map by a
fixed directional term, and everything came out flat and dark.

## The lamps

Sectors carry `eCStaticPointLight_PS`, and nothing was reading it. A light is

    Color        bCFloatColor   four bytes of tag, then red, green and blue
    Range        float          world units - a thousand is ten metres
    CastShadows  bool
    Offset       bCVector       moves the light off the entity's origin

Across all 2177 sectors there are **588 of them**, every one casting shadows, with
ranges from 300 to 4000 units. 588 is small enough that a forward renderer is the
right shape here rather than a deferred one - and our foliage is alpha-tested,
which deferred handles worst.

Each frame the nearest sixteen are written to a uniform buffer and the shader adds
them to daylight. A light is dropped past six times its own range.

![A room lit by its own candle](world-lit.png)

## The curve, and a correction

This note used to say the diffuse maps carry baked shading, which is why the
forests turn black when lit properly. That was wrong, and the game ships the
proof: `Materials.pak` holds the engine's own HLSL, and `ip_hdri.fx` ends every
frame with

    cOut = 1 - exp(-fExposure * cOut)
    cOut *= pow(1 - dot(centre, centre), fVignette)
    cOut = pow(cOut, fGamma)

`ge3.INI` sets `Render.HDRExposure=2.85`, `Render.HDRGamma=0.60`,
`Render.HDRVignette=0.30`. A texel of 0.15 comes out of that at **0.53**. The
darkness was in the curve we were not applying, not in the textures - whose
brightness sits inside the physical albedo band: ground 0.29, rock 0.24, foliage
0.20, bark 0.15.

Exposure and gamma are in, with the game's own constants. Mean frame brightness
over the valley went from 43.7 to 109.5.

![The valley through the game's own tone curve](world-tonemapped.png)

## What the materials can and cannot support

From the same shipped shaders, over all 1150 materials:

- **No metallic and no roughness anywhere.** Not one material binds a texture to
  `SpecularPower`; shininess is a single scalar times 128, a Phong exponent.
- The surface model is albedo times Lambert, plus a normalized Phong lobe tinted
  by the **light** colour rather than the albedo, masked by the specular map.
- 334 of the 736 wired specular slots are not a map at all but
  `Multiply(diffuse, constant)` - the mask is the albedo scaled.

So a physically based layer here has to **invent** roughness rather than read it.
That is worth doing as a separate switch, after the lightmaps, so there is
something to compare against.

## Lightmaps: what is established, and what is not

`Lightmaps.pak` holds **11234 `.xlmp` files, 132 MB**, named
`<mesh>_{guid}.xlmp` - one per placed instance rather than per mesh. That is the
missing contrast: the meshes themselves cannot carry it, because they are shared
between thousands of placements.

Established by reading:

- It is a `GENOMFLE` container, the same wrapper the sectors and meshes use, and
  our own `StringTable` and `readPropertySetHeader` parse it.
- The class is `eCResourceLightmap_PS`, carrying one property, `ResourcePriority`
  (0.04 in the file examined).
- Its string table names **the mesh it belongs to**, in full:
  `/Data/_compiledMesh/G3_Objects_Myrtana_Buildings_01/G3_Architecture_Bridge_Large_01.xcmsh`.
- The body holds prefixed arrays - a one-byte prefix, a u32 count, then the data
  - and the first array's count is **307, exactly the vertex count of that
  mesh's first element**. Its entries are packed four-byte values whose top byte
  varies from 0xf5 to 0xfe: bright, as an outdoor bridge should be. The second
  array is 307 floats, the first of which is exactly 1.0.

Not established:

- What follows those two arrays. The next bytes are 16-byte records repeating
  `0, 1.0, 0, 0`, and neither of the mesh's other element counts (2573 and 3354)
  appears anywhere as a u32 in the first 20 KB - so the remaining elements are
  not stored the same way, and the file is not simply a list of per-element
  vertex colours.
- Which of the two arrays is the light and which is the occlusion, and in what
  space.
- How the instance GUID in the file name binds to the sector entity.

`g3lightmap` reads what is understood and stops there rather than guessing.

## Vertex stream 5

Every element of every mesh carries stream 5 - 9299 of 9299 - and a review
suggested it holds per-vertex ambient occlusion. Read across all 11224508
vertices it is **zero**: mean 0.000, none at full brightness. The contrast is in
the lightmaps and there is nowhere else it could be. 2263 elements carry a second
UV set, which is what addresses them.
