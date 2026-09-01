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

**It is a directional lightmap.** The second array is not floats but unit
vectors, twelve bytes each: over two thousand of them sampled, every single one
has a length of 1.0000. So each vertex carries a colour and the direction the
light came from - which is what a normal-mapped surface needs and a flat colour
cannot give. The pair is followed by the marker `44332211`, the same shape the
sector records use.

The decisive test was not the bytes of one file but three files of one mesh. The
arena appears three times with different GUIDs and different sizes - 440280,
245138 and 443880 - and all three carry the same count, 11285, which is exactly
the vertex count of that mesh's first element. Their contents differ where they
should: one has colours with all four bytes set, the others only the top byte,
and a scalar in the header reads 1.0, 0.84 and 0.28. Per instance, as the file
names say.

`g3lightmap` reads that, and **2779 of the 11233 files parse - 3778454 lit
vertices**. The remaining 8454 fail at the end marker, so a mesh with more than
one element carries more than this, and the walk stops where it stops being sure.

Still not established:

- What follows for the other elements, and what the remaining bytes hold - 148565
  of the bridge's 153723 are still unread.
- Whether the colour is the light itself or light times albedo, and in what
  space.
- How the instance GUID in the file name binds to the sector entity.

## Vertex stream 5

Every element of every mesh carries stream 5 - 9299 of 9299 - and a review
suggested it holds per-vertex ambient occlusion. Read across all 11224508
vertices it is **zero**: mean 0.000, none at full brightness. The contrast is in
the lightmaps and there is nowhere else it could be. 2263 elements carry a second
UV set, which is what addresses them.
