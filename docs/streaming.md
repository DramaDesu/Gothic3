# Streaming: which sectors are worth having

The world is 2177 sectors and 347 MB of static geometry. Loading all of it takes
21.8 seconds and leaves 1.34 million instances for the cull to walk every frame.
Almost none of it is visible. This is how the game decides what to keep, and how
we read that decision out of it rather than inventing one.

## The rule the game uses

Gothic 3 streams through a class called `eCPVSPrefetcher3`. The name promises
potentially visible sets; the implementation has none. There are no portals, no
cells-and-portals visibility, no precomputed PVS. What it actually does is:

- lay a uniform grid of **10000-unit cells** over the world;
- keep a rectangle of cells around the camera whose half-width is
  **four tenths of the far clipping plane**;
- widen that rectangle by **one cell on every side**;
- keep a sector resident while **its own bounding box** overlaps the rectangle.

The vertical axis takes no part. Residency is decided in the ground plane alone,
which is what you would expect of a world that is a landscape rather than a
building.

Cell indices are computed on biased coordinates:

```
cell = (int(coordinate) + 1000000) / 10000
```

The million is there so the division floors correctly for negative coordinates
without anyone having to think about it. Both numbers are the game's, not ours.

`Render.PrefetchGridCellSize` exists as a console variable and setting it does
nothing: the cell size is compiled in. It is the sort of thing worth checking
before building a feature on top of it.

### The box, not the name

Sectors are named for their cell - `x55000z55000` - and it is tempting to use
the name as the position. It is wrong. The box of `x55000z55000` begins at
x = 45000: a full cell outside its own name. Deciding by name would drop
geometry that is directly in front of the camera.

The box lives beside each sector as a `.lrgeodat`: exactly 197 bytes, six floats
at offset `0x4F`, minimum then maximum. The box appears twice in the file - once
as the property's default and once as its value - and the two are identical
throughout the shipping data, so the first is taken.

That makes the whole residency index **2177 files of 197 bytes**, read in 0.01
seconds, deciding the fate of 347 MB that never gets opened.

### Inverted boxes

66 of the 2177 carry `(+FLT_MAX, +FLT_MAX, +FLT_MAX) .. (-FLT_MAX, -FLT_MAX,
-FLT_MAX)`: a box initialised and never grown by a single vertex. That is an
empty sector saying so. Refusing them is the right answer, not a parse failure -
which is worth writing down, because an inverted box looks exactly like a
misread offset until you check what is inside.

### When it recomputes

Not every frame. The set is recomputed when the rectangle has changed **and**
the camera has moved at least `max(1300, farClip * 0.2)` since the last one. The
first condition is the cheap test; the second is what stops a camera sitting on
a cell boundary from thrashing a sector in and out. We do not implement the gate
yet - residency is chosen once, at startup - but it is the game's number and it
belongs here.

## What it buys

Measured at the fortress camera, `--stream` against the whole world:

|                     | whole world | resident set |
|---------------------|-------------|--------------|
| loading             | 21.8 s      | **4.8 s**    |
| sectors             | 2177        | **36**       |
| instances           | 1 336 788   | **63 798**   |
| cull                | 13-20 ms    | **1.47 ms**  |
| instances walked    | 0.90 M      | **0.04 M**   |
| lightmaps           | 8.3 s       | 0.7 s        |
| draws               | -           | 932          |
| triangles submitted | -           | 1.84 M       |

Thirty-six sectors and not sixteen, because the boxes overhang their cells. That
is the same fact as "the box, not the name", seen from the other side.

It also lifted a limit the whole-world load could not meet: the baked-light
atlas took 8701 patches of 124940 and refused 116239. The resident set packs all
19046 and refuses none.

## What this is not

This is residency chosen **once**, at startup. Nothing arrives or leaves while
flying, because the vertex and index buffers are still built once in `create()`.
The design for making geometry come and go:

- **Fixed-capacity device-local buffers with free-list suballocation.** Vertices
  and indices are addressed by element offset - `vertexOffset` and `firstIndex`
  in the draw command - so a suballocation inside a buffer that never changes
  needs no descriptor at all. The lighting buffers are read as
  `buffer[base + gl_VertexIndex]`, so they suballocate the same way. Nothing is
  ever rewritten while a frame in flight may be reading it, because nothing is
  ever rewritten.
- **A sector id on every batch and range**, so a whole sector's draws unlink in
  one step rather than being searched for.
- **An incremental grid**, since rebuilding the spatial index from scratch on
  every arrival would cost more than the arrival.
- **One sector a frame, with a time slice**, which is what the game does.

## Files

- `src/genome/residency.h`, `.cpp` - the rule, and the `.lrgeodat` reader.
- `tools/g3world.cpp` - `--stream` filters the load through it.
