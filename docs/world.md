# The world, as far as we have looked

Established with our own archive reader against `Projects_compiled.pak`
(8102 entries: 1376 `.node`, 88 `.lrentdat`, plus per-layer `.sec`/`.secdat`).

## A grid, spelled out in the file names

Sectors are named by their position:

    g3_world_01_x55000y0z55000_cstat.node

- **2177 sectors**, a grid of **62 x 60** cells
- **step 10000 units**, which is exactly the `Render.PrefetchGridCellSize`
  default in `ge3.ini` - the streaming setting and the file layout agree
- extent **6.8 x 6.4 km** at one unit per centimetre, so the fan estimates of
  30-60 km2 are closer to the truth than the 75 km2 the publisher advertised

## Content is split by region and purpose

Top level is `myrtana`, `varant`, `nordmar`, plus `world`, `maps`,
`start_points` and a `sysdyn_{guid}` layer holding the biggest entity file
(80 MB). Inside each region the layers are named for what they carry:
`..._landscape_vegetation_01`, `..._dynamic_objects_03`, `..._landscape_birds_01`,
`..._navzones_01`. Navigation is one 18 MB `navigationmap.xnav`.

`g3_world_lowpoly_01_levelmesh_01` is a single low-poly mesh of the whole level -
the distant backdrop the engine draws past the normal far plane. Being one file
that covers everything, it is the obvious first thing for our runtime to render.

## The landscape draws already

![Myrtana, Varant and Nordmar](world-textured.png)

The low-poly landscape turned out to need no placement data at all: those tiles
are ordinary `.xcmsh` meshes whose vertices are already in world space, tiled on
a 30000-unit grid that lines up with the sector names. So the whole map is just
349 meshes loaded and concatenated - 397k vertices, 389k triangles, spanning
4.9 x 5.0 km. Each tile names a regional material, and those resolve to just **17 distinct
textures** for the whole map - Myrtana, Varant and Nordmar in a few variants
each - so they are uploaded once and shared, and the map draws as a handful of
texture binds rather than one per tile. The diffuse maps already carry baked
shading, which is why the lighting here stays gentle: at the strength a
character wants, the forests turn black.

    g3world "…/Gothic 3/Data/_compiledMesh.pak"

## Not yet established

The binary layout of `.node` and `.lrentdat`, how a sector lists its entities,
how an entity names its `.xcmsh`, and whether the terrain proper is meshes or a
height field. Three agents were mapping exactly this when the session ran out;
the reference is g3dit's Java parsers under `LrentNode/`.
