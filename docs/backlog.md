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

**Step-up works and is barely tested.** Withdrawing the claim that stood here:
the world does place stairs, and the search that said otherwise was simply a bad
one - it looked at the visual mesh names a sector lists, and found them once it
looked at the collision file each placement names instead. There are stairs at
`59403 7899 58444` and half a dozen other places in the streamed rectangle.

Walking at one of them took a step for the first time, and the step lifted the
body by 97 against a threshold of 70: raising the capsule and letting it settle
can leave it standing well above what the threshold allows, so a climb passed as
a stride. Clamped now. What is still missing is a clean approach - a body
dropped near a staircase spends the run falling rather than walking, so the
positive case rests on that single observed step.

## The character controller

**The walk stalls on slopes, and the step logic is not what fixes it.** A
measured walk-forward into the tallest step the finder reported covered 1211 of
the 2100 it asked for and then stopped dead. The stack at the stall is a single
face with normal (0.15, 0.76, -0.64) - a forty-degree ramp. Walking is purely
horizontal, so on a ramp the whole step is spent pushing into the surface and
being pushed back out.

Two facts to keep. The step attempt is gated on "covered less than it asked
for", which is true on *every* slope: 247 attempts and none taken in that run,
two wasted queries a frame. And the climbing that did happen, 6965 to 7184, came
from the collision resolve sliding the body up rather than from the step logic,
which reported none.

Projecting movement onto the ground plane is the textbook fix, and a first
attempt made it worse - covered fell from 1211 to 324. That attempt changed the
projection and the step gate together, so it says nothing about either, and was
reverted rather than committed. The next try should change one at a time, and
should check whether `support` is even the surface being stood on: it is updated
only when a standable contact is the deepest one, so at a wall-and-floor corner
it can be a frame stale.

## Renderer

- The CPU-side `genome::Image` for a texture is never freed after upload.
- A sector that was refused is retried on every change of the streamed
  rectangle, rather than being remembered as refused.
- Eight tree kinds end up with no billboard and keep the thinned mesh to the
  horizon.
- Rendering offscreen above the desktop's size does not work.
- Synchronization validation has never actually been run - only the standard
  validation layers.
