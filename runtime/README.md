# Genome runtime

Our own 64-bit renderer for Gothic 3 assets. It does not touch the game process:
it reads the archives directly, so it is free of the original engine's 32-bit
address space, DirectX 9 pipeline and single-threaded tick.

It reads the archives and the compiled static meshes. Next comes the actor
(skeleton and skinned mesh), then a character standing in bind pose.

## Build

    cmake -S runtime -B runtime/build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build runtime/build

## Try it

    g3pak  list    "…/Gothic 3/Data/_compiledMesh.pak" orc
    g3pak  extract "…/Gothic 3/Data/_compiledAnimation.pak" g3_orc_body_warrior.xact orc.xact
    g3mesh "…/Gothic 3/Data/_compiledMesh.pak" g3_weapon_orc_sword_01.xcmsh
    g3mesh "…/Gothic 3/Data/_compiledMesh.pak"      # parse every mesh, the real test

## What the data looks like

| archive | entries | holds |
| --- | --- | --- |
| `_compiledMesh.pak` | 6292 | `.xcmsh` static meshes, including `_col` collision variants |
| `_compiledAnimation.pak` | 6324 | 387 `.xact` actors (skeleton + skinned mesh) and `.xmot` motions |
| `_compiledImage.pak` | — | `.ximg` textures |
| `_compiledMaterial.pak` | — | `.xshmat` materials |

Motion file names encode the whole state machine, which is a free description of
the animation graph:

    wolf_stand_none_none_p0_warn_begin_n_fwd_00_%_00_p0_0.xmot
    <actor>_<pose>_<weapon>_…_<phase>_<direction>…

An orc has one actor per rank (`g3_orc_body_warrior.xact`, `_elite`, `_boss`,
`_scout`, `_shaman`) and 942 motion files match `orc_`.

## Archive format

48-byte header, then a depth-first file table: each entry carries three
filetimes, an attribute word (`0x10` marks a directory), and for files a 64-bit
offset/stored-size/size triple plus zlib flags. Names are length-prefixed with a
trailing terminator that the length does not count.

`.p00`/`.p01` volumes are overlays patching the base archive in order, so a
Community Patch file wins over the original; a zero-sized overlay entry is a
deletion. The reader applies them at load time, which is why the mesh archive
reports 6292 usable entries out of 6300.

## Mesh loading

`g3mesh` with no name parses the whole archive: **5402 meshes, none failing**,
11.2M vertices and 7.5M triangles. The check is strict - every file has to end
exactly where its header says the body ends, so a silent misparse fails loudly
instead of producing plausible garbage.

A mesh is a list of elements; one element is one material and one draw call,
with its own index buffer numbered from zero. Vertex data arrives as separate
streams (position, normal, tangent, uv, and a "diffuse" stream that is really
bitangent handedness) which are parallel arrays and can be interleaved straight
into a vertex buffer. Streams appear in no fixed order, so they are dispatched
on their type rather than their position.

The format has two flavours in the same archive: files wrapped in `GENOMFLE`
address strings as two-byte indices into a table at the end of the file, while
raw files store them inline. That is sniffed per file - assuming either one is
the fastest way to misparse everything.
