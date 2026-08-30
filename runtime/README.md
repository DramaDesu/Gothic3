# Genome runtime

Our own 64-bit renderer for Gothic 3 assets. It does not touch the game process:
it reads the archives directly, so it is free of the original engine's 32-bit
address space, DirectX 9 pipeline and single-threaded tick.

It reads the archives, the compiled static meshes and the animated actors -
skeleton, skinned mesh and weights. Next comes motion playback, then a window.

## Build

    cmake -S runtime -B runtime/build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build runtime/build

## Try it

    g3pak  list    "…/Gothic 3/Data/_compiledMesh.pak" orc
    g3pak  extract "…/Gothic 3/Data/_compiledAnimation.pak" g3_orc_body_warrior.xact orc.xact
    g3mesh "…/Gothic 3/Data/_compiledMesh.pak" g3_weapon_orc_sword_01.xcmsh
    g3mesh  "…/Gothic 3/Data/_compiledMesh.pak"      # parse every mesh, the real test
    g3actor "…/Gothic 3/Data/_compiledAnimation.pak" G3_Orc_Body_Warrior.xact
    g3actor "…/Gothic 3/Data/_compiledAnimation.pak" # parse every actor

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

## Actors

`.xact` is a different format from the meshes: past the same outer wrapper it is
an EmotionFX-2 chunk stream rather than a property set. 386 of 387 shipped
actors parse, giving 39159 bones and 1.09M vertices.

The orc we are aiming at loads as expected:

    G3_Orc_Body_Warrior.xact
    123 nodes, 1 submesh, 8176 vertices, 10206 triangles
    bind pose spans 173.1 x 203.7 x 58.3 cm
    2 roots, 5276 skinned vertices, up to 4 influences each
    material: G3_Animation_Orc_Warrior_01.xshmat

Bones name their parent rather than indexing it, and the array is not sorted, so
parents are resolved by name and only then composed into global bind matrices.
Mesh vertices are already in bind space - applying the mesh node's transform is
a tempting mistake that moves a head 167 units off its own skeleton.

Two head actors declare a scene-info chunk smaller than its contents. A strict
size-driven walk desyncs there and then reads a garbage chunk id, so that chunk
is parsed and the later of the two ends is used.

## Next: motions

`.xmot` files address a *cleaned* skeleton: the engine folds away helper nodes
whose names carry more than two underscore-separated parts (`*_ROOT`, `*_END`)
before animating. Driving the raw hierarchy with motion values double-counts
those transforms - measured elsewhere as 10-91 cm of error per bone. The cleanup
has to happen first, keeping a map back to the original indices so skinning
still resolves.
