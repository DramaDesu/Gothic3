# Open items

What is deliberately unfinished, and why. An item earns a place here by being
something we found and chose not to do yet - not by being imaginable.

## Decisions waiting

**The canopy should probably not be solid to the player.** The engine keeps the
trunk and the branches in separate shape groups - `eEShapeGroup_Tree_Trunk` and
`eEShapeGroup_Tree_Branches` - so they are not the same kind of obstacle: a
player walks into the first, and an arrow is stopped by the second. We read and
draw both today, which is why a wood reads as a field of blocks. The lean is to
give the player the trunk only and keep the canopy for whatever else wants it.
Not decided, and cheap to change once something actually collides.

**Which physics engine, or none.** Deferred on purpose: no modern engine can
load Gothic 3's cooked shapes, since PhysX 3.0 broke compatibility with the 2.x
format and never went back. The triangles have to be recovered by parsing
whatever we choose, and once recovered, feeding them to any engine is the same
work - so the parsing was the real task and it is done. box3d, Jolt and writing
our own are all still open.

## Collision

**Use the named mesh, not only the rule.** We read the entity's own reference
now, and it checks out - 4012 named files across four rectangles, all resolving,
and zero disagreements with the suffix rule. So there is no correctness pressure
here any more, only tidiness: the lookup still goes through the rule, and the
`u16` that selects one sub-mesh inside a cooked file is read and ignored. Worth
switching when something needs the sub-mesh.

**Scaled variants have a naming rule we do not follow.** A cooked triangle mesh
cannot be scaled at use, so the game cooks a variant per scale and names it with
the scale baked in - `_SC_%.4f` uniform, or `_SCX_%.4f_SCY_%.4f_SCZ_%.4f`, with
`.` written as `_`. There are 471 such entries in `_compiledPhysic.pak`. Convex
hulls are always the uniform form; triangle meshes take the three-axis form
unless the entity is a node entity. We apply the world matrix instead, which is
right for drawing and would be wrong for a real cooked-mesh physics body.

**The navigation map is a second, unread source of where you may walk.**
`g3_world_01/navigationmap.xnav`, 18 MB, holds 74145 obstacle circles and 795
zones. Roughly half the world's trees have an obstacle there and the other half
- every bush, the extra-small buckthorns, both hollies - have none at all, which
is a statement about what the game considers an obstacle that our per-tree
shapes do not make.

**Building the collision trees costs 180 ms at startup.** One-off, and on the
startup path: 534 meshes, 565872 triangles. The worker pool is right there and
each mesh is independent of every other, so this is a fan-out waiting to happen.
A streaming rebuild is 6 ms and does not need it.

**We may not be loading every layer of the world.** Looking for a staircase to
test the controller's step-up against found no placement named for stairs in any
`.node` file we read, although `_compiledPhysic.pak` holds a dozen cooked stair
meshes. Either the architecture lives in a layer we skip, or it is placed under
names we did not think to look for. Worth settling: it decides whether the world
we walk is the whole one.

**Step-up is unconfirmed in the positive case.** It refuses correctly - every
refusal measured was a probe returning to the height it left, which is a wall -
but it has never been seen to succeed, because no walk met a real step. Follows
from the item above.

## Renderer

- The CPU-side `genome::Image` for a texture is never freed after upload.
- A sector that was refused is retried on every change of the streamed
  rectangle, rather than being remembered as refused.
- Eight tree kinds end up with no billboard and keep the thinned mesh to the
  horizon.
- Rendering offscreen above the desktop's size does not work.
- Synchronization validation has never actually been run - only the standard
  validation layers.
