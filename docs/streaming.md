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

## What it does now

Sectors arrive and leave while flying. The rectangle changing is the cheap test;
the distance gate is what stops a camera sitting on a cell boundary from
thrashing one sector in and out. Departures go first, because they are what
makes room for the arrivals, and then one sector arrives per frame.

Flying 600 frames from the fortress:

```
22 sectors arrived and 24 left while flying; 35 resident now
arrivals cost 141 ms at worst and 29 ms on average, 645 ms in all
arenas: 48% -> 49% of 5M vertices, 48% -> 49% of 10M indices
```

Over a longer run - 147 arrivals and 151 departures - the geometry arenas stay
at 48%. That is the free lists doing their job: what a sector took it gave back,
and nothing crept.

### It has to land where a cold load would

Flying into a place and loading it there must give the same world. Measured at
one spot, reached by flying 300 frames and then loaded from cold at the same
camera: 71842 instances against 71860, and the same forest tree for tree.

Getting the trees to match took a fix. A tree's variant - one of three seeds per
definition, so that a wood is not one tree repeated - was chosen by a running
count of trees planted, which makes the forest depend on the order sectors
happened to be read. It comes from a hash of the tree's position now, so the
same tree stands in the same place however it was reached.

The 18 instances still missing are billboards: 13 tree kinds first appeared
after the billboard atlas had been baked, and a kind with no cell in that atlas
keeps its thinned mesh all the way out instead. Worse to look at, correct to
draw, and counted out loud rather than left to be noticed.

## What an arrival costs

It was 29 ms on average and 141 ms at worst, and the plan was to move the load
off the main thread on the assumption that the load is the expensive part. It
was half of it. Measured across 44 arrivals, 1107 ms in all:

```
lightmaps              421 ms   38%   disk and CPU
waiting on the queue   324 ms   29%   vkQueueWaitIdle in endOneShot
patch tiles             69 ms    6%   GPU, one submit and one wait per tile
reading sectors         67 ms    6%
meshes                  57 ms    5%
rebuilding the grid     54 ms    5%   CPU, inside addSector
textures                30 ms    3%
building vertex arrays  11 ms    1%   CPU, inside addSector
trees                    6 ms    1%
```

Neither of the two big numbers would have been guessed right. Half of it was
loading; a third of it was the queue.

A benched flight uses a fixed sixtieth-of-a-second step so that the same command
flies the same path every run - the same 28 arrivals and 36 departures every
time - because two measurements taken at different distances cannot be compared.

## Four things, in the order they were done

**The atlas tiles went across together.** Each was its own staging buffer, its
own pair of layout transitions, its own submit and its own drain, and a sector
brings about ten. One command buffer now: one barrier in, N copies, one barrier
out. 1.6 ms an arrival to 0.3.

**The queue stopped being drained.** `endOneShot` submitted and then called
`vkQueueWaitIdle`, which waits for everything on the queue - including the frame
being rendered. There is one queue, so submissions execute in submission order:
a transfer submitted before the frame's draws runs before them, and a pipeline
barrier at the end of the transfer command buffer puts those later submissions
in its second synchronization scope. So nothing needs waiting for. The
submission is fenced only so the staging buffer can be recycled once the copy
has really happened, and at most eight are left in flight. The uploader stopped
keeping a 64 MB staging buffer and now makes one the size of what it holds -
reusing one is safe only while the copy is waited for.

**Two bugs the audit found, one of them made by the change above.** dropSector
released arena ranges the instant a sector left, which was safe only because the
drain covered it; with the drain gone, an arriving sector could overwrite
vertices a submitted frame was about to draw. Ranges and freed atlas tiles now
wait out the frames in flight. And PatchAtlas kept its shelf cursor across
sectors, so an arriving sector could pack into a tile another sector owned:
those patches never reached the texture, and the tile was freed under the sector
still sampling it.

**The load moved to a thread of its own.** It cannot be shared:
`PakArchive::read` seeks a `FILE*` it holds, and every cache is a plain
`std::map`. So one thread owns all of it - the archives, the mesh and tree
caches, the images, the patch atlas - and nothing else calls in. The main thread
posts a path and takes back a finished sector, including the pixels of the atlas
tiles it filled, and does the Vulkan half only. A sector that finishes after the
camera has left is dropped; giving its tiles back is all the undoing needed.

Then the spike moved instead of vanishing: with loading off the frame the worst
frame went UP, 142 ms to 237, because departures arrive in a burst when the
rectangle changes and each `dropSector` rebuilt the extents, bounds, occluders
and grid over every batch. Those views are marked stale now and rebuilt once,
before the cull that reads them.

## Where it ended up

Over the fixed 900-frame flight, 28 arrivals and 36 departures:

|                | before  | after  |
|----------------|---------|--------|
| on the frame   | 727 ms  | 78 ms  |
| an arrival     | 26 ms   | 3 ms   |
| worst arrival  | 139 ms  | 9 ms   |
| worst frame    | 142 ms  | 19 ms  |

Against a 16.7 ms refresh, a 19 ms frame is not a hitch. The loader spends 663
ms of its own - 62 reading sectors, 92 meshes, 462 lightmaps, 8 trees, 39
textures - none of it on the frame.

The world it builds is identical, not merely equivalent: the screenshot after a
threaded flight matches the synchronous one byte for byte.

## Textures: where the wall was not

The plan was to make textures come and go with their sectors, because they are
created when first seen and never released, each takes a VkDeviceMemory of its
own, and the descriptor pool is a fixed number of sets. Measuring it took the
reason away.

```
maxMemoryAllocationCount   4294967295 on this driver
allocations                203 live, 208 at peak
memory                     599 MB live, 616 MB at peak
textures                   184 at load, 192 after 127 arrivals
```

The allocation count is not a limit here at all. The whole game ships 1897
images and the pool holds 8192 sets, so every image in it could be resident at
once and still fit. What is left is 599 MB, which is a memory question to be
decided against a budget, not a lifetime one to be forced by a limit.

The counter that says so was wrong the first time and the mistake is worth
keeping: createBuffer and the depth image call vkAllocateMemory directly rather
than through Device::allocate, so buffers decremented a count they had never
incremented and it sat at zero - 9 at peak with 210 textures live.

What the audit did find was one live bug and one piece of waste.

**A stale index.** dropSector erases from m_batches, shifting every later index,
and marks the derived views stale without touching m_batchOf; addSector reads
m_batchOf first and rebuilds last. A sector arriving on a frame another leaves
therefore indexed m_batches with pre-erase indices. Counting what the lookup
would have returned: at 2500 units a second it never happened, and at 12000 one
arrival landed on such a frame and 92 lookups would have named the wrong batch
with 19 one past the end. `ensureDerived()` at the top of addSector closes it.

**One file, one image.** The image cache was keyed by material name rather than
by the file the material resolves to, so materials sharing a diffuse map each
loaded their own copy and got their own image, allocation and descriptor set.
Archive-wide, 913 materials resolve to 634 distinct files, one named twenty-five
times. On the fixed flight: 211 textures at load became 184, 235 allocations
became 203, and 633 MB became 599 MB.

Three quiet failure paths went with it. findMemoryType returned 0 when nothing
matched, and 0 is a real memory type - on this device host memory that is still
usable for optimally tiled colour images, so a mask miss would have put a
texture meant for the card into system RAM and said nothing. createTexture
leaked a VkImage on exactly the failures memory pressure produces, and never
checked whether the image view was made before writing it into a descriptor set.

Over 4000 frames at 3500 units a second - 127 arrivals, 154 departures - the
worst frame is 12.8 ms and an arrival costs 1 ms on the frame.

## What it still does not do

- **Textures are still never freed.** Not a wall, as above, but 599 MB. If they
  are ever evicted the descriptor pool needs
  VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, the sets have to wait out
  the frames in flight the way arena ranges do, and there is currently no route
  from a genome::Image to its Texture to free.
- **A refused sector is retried every time the rectangle changes**, and fails
  again for the same reason. Harmless, since the unwind is clean, but it is
  work done for nothing.
- **Billboard cells are not suballocated.** The atlas is baked once, from the
  kinds the first resident set happened to use. A fixed cell grid with cells
  handed out as kinds appear is the same trick used everywhere else here.
- **Lights are gathered once.** 588 across the world, so nothing is visibly
  wrong yet, but they should come and go with their sectors.
- **Synchronization validation has never actually run here.** The unwaited
  uploads are correct by the spec rule that a barrier orders against later
  submissions on the same queue, and that is an argument rather than a check. It
  was asked for by name in the instance chain, the environment variable that is
  supposed to enable it did nothing, and deliberately removing the barrier to
  see the check fire produced no complaint either - so the check is off, not
  passed. Getting it to run is worth a session of its own.

## How the pieces sit

- **Geometry** lives in fixed-capacity device-local buffers, suballocated per
  mesh and refcounted, because a crate is a crate in every sector. Vertices and
  indices are addressed by element offset in the draw command, so nothing points
  at them that would have to be rewritten.
- **A batch is keyed by mesh, and an instance carries its sector.** Keying
  batches by sector was the obvious move and the wrong one: fifty sectors
  holding the same crate would mean fifty draws for something that was one.
- **Baked light** goes into one allocator serving three buffers, because the
  shader reads colour, incident direction and patch coordinate from the single
  base an instance carries.
- **Baked patches** go into atlas tiles of 128, which is the unit a sector hands
  back. Arrivals write their tiles into the atlas texture directly; the image
  and its descriptor never change.

## Files

- `src/genome/residency.h`, `.cpp` - the rule, and the `.lrgeodat` reader.
- `tools/g3world.cpp` - `--stream` filters the load through it.

## The two shots

`docs/world-streamed.png` was flown to; `docs/world-cold.png` was loaded there.
Same trees, same trunks, same rocks, same fern. The slight shift is the camera:
the flight prints its angles to two decimals and the cold run was given those.
