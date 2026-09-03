# The collision geometry

The game ships what it collides with separately from what it draws, and that
turns out to be the single most useful fact about making a player walk in it.

`_compiledPhysic.pak` holds 6735 files with the extension `.xnvmsh`, one per
object, and the mesh archive holds 782 more. A chest is 682 bytes. The median is
9691. They are far simpler than the meshes beside them: a grindstone wheel is 14
triangles in collision and 14 in the visual, but a castle is 98152 against
hundreds of thousands.

## What a file is

An ordinary Genome property set - the same container already read for
`.lrgeodat` and `.xlmp` - of class `eCResourceCollisionMesh_PS`, carrying the
object's name and a `ResourcePriority` float, wrapped around one or more cooked
NovodeX meshes. NovodeX is what PhysX was called before NVIDIA bought it, and
Gothic 3 shipped on PhysX 2.x in 2006.

Each blob:

```
+0   "NXS", a version byte, "MESH"
+16  the float 0.001, identical in every file - the cooking tolerance
+28  the vertex count
+32  the triangle count
+36  the vertices, three floats each, then the indices
```

Then an OPCODE tree, which was its acceleration structure. That is skipped: we
build our own or hand the triangles to something that does.

The index width is not written down. It is 8, 16 or 32 bits and it varies within
a single file - a flight of stairs has one part at 32 bits and its neighbours at
16. So each width is tried and the one that reads correctly is taken.

A file holds several blobs when the object was authored in pieces: a stick is
two, a flight of stairs three, most objects one.

## What is in there

| | triangle meshes | convex hulls |
|---|---|---|
| `_compiledPhysic.pak` | 3940 | 2795 |
| `_compiledMesh.pak` | 782 | 0 |

The hulls are `NXS` + `CVXM` and their names end in `_cv`: helmets, armour, the
things thrown about rather than stood on. They are not read yet, because walking
needs the triangle meshes. Nothing else is in there - the third bucket is empty,
so the reader accounts for every file.

7.15M triangles for the whole world's collision, median 184 an object. That is a
different scale from drawing it, where a single sector can place 1.4M.

The landscape has collision per cell:
`g3_myrtana_landscape_01/lod/g3_myrtana_landscape_cell_243.xnvmsh`.

## How the reading was checked

Three checks, and each one caught something the one before it could not.

**Every index inside its own vertices.** This is the obvious test and it is not
enough. Reading 16-bit indices as 8-bit leaves every second byte a zero, and a
zero is perfectly in range - so the mesh passes while half its triangles come out
as a corner repeated.

**No degenerate triangles.** This is what tells the two apart, and it is
unambiguous: a right reading has none and a wrong one has half. It caught the
index width immediately, on files where the first test had been happy.

**The bounding box against the visual mesh.** Both of the above are internal:
they check the reading against itself, so they cannot catch a reading that is
self-consistent and still not the object. The mesh archive is an independent
source, and comparing the two boxes failed completely - 0 of 1026 agreed, worst
99% off.

That failure was the most valuable result of the day. The collision is in
**metres** and everything drawn is in **centimetres**: a grindstone wheel is
0.259 across in collision and 25.9 drawn, with 14 triangles on each side and four
parts against four elements. PhysX works in SI units and the cooking kept them;
the renderer never had to care. With the factor applied, 1025 of 1026 agree and
the ratio of sizes is exactly 1 at the 10th, 50th and 90th percentile.

Without that check the player would have collided with a world a hundred times
too small, and every internal test would have said the reading was fine.

## What this means for the physics engine question

No modern engine can load this data. PhysX 3.0 broke compatibility with 2.x
cooked formats and never looked back, so PhysX 5 cannot read it either - the
option with a claim to authenticity has no advantage at all. The triangles have
to be recovered by parsing whatever engine is chosen, and once they are
recovered, feeding them to any engine is the same work.

So the decision can be deferred, and the parsing could not be.
