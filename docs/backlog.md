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

**Walking up a slope works now; walking into things still does not.** The stall
was never a stair. The surface at it is a single face with normal
(0.15, 0.76, -0.64) - a forty-degree ramp - and movement was purely horizontal,
so the whole step went into pushing at the surface and being pushed back out.

Two changes fixed it, and it took three attempts to find out which. Movement is
projected onto the plane underfoot, and the ground snap that catches a body
walking off a tread was moved out of the `else` it sat in - on a slope the step
attempt fires every frame and was swallowing the snap with it, so the body left
the ground and never came back. Either alone is worse than neither: projection
without the snap launches the body and it walks on air, covering 448 where it
had covered 1211.

Measured on the same walk: **1211 of 2100 and stalled, against 3991 of 5250 and
still climbing.**

Two hypotheses tested and refuted along the way, both worth not retesting.
`support` was suspected of being a frame stale, since it was only updated when a
standable contact was the deepest; it is now the most upward-facing standable
normal instead, which is more correct but changed no measurement - the value was
already right here. And the step probe was suspected of falling short: one
measured refusal probed from 4207 with 175 of reach, stopping at 4032, with the
ground it wanted at 4024. Starting the probe at the feet rather than a step
above them fixes that arithmetic and made everything worse - 3991 down to 784 -
so the eight-unit miss was not what the refusals were about.

**The open-ground stall was a tree.** A walk across the meadow covers 565 of
3500 and stops at 52994 3971 50323, and the picture from behind the character
shows him pressed against a trunk that fills half the frame. Walking straight
into a cylinder and stopping is the right answer, and it is also the first proof
that the tree collision read out of the `.spt` primitives actually stops the
player rather than merely drawing.

What is still open is what a body should do *alongside* an obstacle rather than
head-on. The resolve removes the component into the surface, which is sliding,
but nothing has been measured at an angle - the walk-forward harness only ever
walks one way. A harness that walks a few headings from the same spot would say
whether sliding works or whether it only looks like it should.

## Renderer

- The CPU-side `genome::Image` for a texture is never freed after upload.
- A sector that was refused is retried on every change of the streamed
  rectangle, rather than being remembered as refused.
- Eight tree kinds end up with no billboard and keep the thinned mesh to the
  horizon.
- Rendering offscreen above the desktop's size does not work.
- Synchronization validation has never actually been run - only the standard
  validation layers.
