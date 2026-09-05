# Open items

The corrected plan is `docs/plan.md`; this is the detail behind its items.

What is deliberately unfinished, and why. An item earns a place here by being
something we found and chose not to do yet - not by being imaginable.

## Decided, and recorded here so the reasoning stays

**The canopy is no longer solid, and the forest opened up.** This was the open
decision here and the measurement settled it. Walking eight headings from one
spot in a wood covered 400 to 650 of the 1750 asked; the same walk with tree
collision left out entirely covered 1741 and 1866. A canopy sphere has a radius
of about 27 per cent of the tree's height, so a wood is a field of overlapping
balls at head height and nothing can get through.

The trunk stays solid and the canopy is drawn but not collided with, which is
what the engine's own split into `eEShapeGroup_Tree_Trunk` and
`_Tree_Branches` says to do. Measured after: the same three headings cover 1741,
1884 and 1735, and a heading aimed at a trunk still stops at 496. The trunks are
demonstrably still there - the solid world holds 473 meshes with trees against
440 without, a difference of 66120 triangles.

What is deliberately not decided: whether anything else should collide with the
canopy. An arrow probably should, and there is nothing to shoot yet.

**Which physics engine, or none.** Deferred on purpose: no modern engine can
load Gothic 3's cooked shapes, since PhysX 3.0 broke compatibility with the 2.x
format and never went back. The triangles have to be recovered by parsing
whatever we choose, and once recovered, feeding them to any engine is the same
work - so the parsing was the real task and it is done. box3d, Jolt and writing
our own are all still open.

## Collision

**Load the collision the entity names - the rule is wrong four times in five.**
An earlier version of this item said the rule and the reference agree
everywhere, on the strength of a counter that could not disagree: it counted a
mismatch only when the named stem was none of `""`, `_col` or `_cv`, so naming
`_cv` while the loader takes `_col` was "agreement" by construction. An
independent parse over 165 sectors puts it at **81% of placements with a named
shape getting a different file than they name** - 10836 of 13333 - always finer
than the game uses. The fix is in `docs/plan.md`, fix 6.

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
a stride. Refused now - a step that settles above 70 is treated as a wall, which means the one observed 97-lift step is exactly what the code rejects. What is still missing is a clean approach - a body
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

## Characters

**A character costs 24 us a frame, and two thirds of it is the GPU.** The
attribution to posing below was written from whole-frame medians; the fence
phase says otherwise - host-visible vertex and index buffers, no cull on the
character path. The order of fixes is in `docs/plan.md`, build 3.

**The posing third is still mostly wasted.**
samplePose and skinningMatrices run for every piece every frame and the whole
bone palette is uploaded, whether or not anything moved. In the test room 0, 24
and 96 characters cost 0.71, 1.25 and 3.00 ms, so the world's 16604 would be
four hundred. A bind-posed NPC never changes; even a moving one only needs its
own slice rewritten.

**NPCs stand in their bind pose.** A clip belongs to a skeleton, so the hero's
walk cannot pose a goblin. What an NPC should play is its own idle, found the
way the hero's three were - by name, in the animation archive - and that lookup
is the next piece of work.

**Wearing an actor is not being a person.** eCVisualAnimation_PS is on 16604
entities including animated chests; gCNPC_PS is on 10279 and is the test for
someone to talk to.

## Renderer

- The CPU-side `genome::Image` for a texture is never freed after upload.
- A sector that was refused is retried on every change of the streamed
  rectangle, rather than being remembered as refused.
- Eight tree kinds end up with no billboard and keep the thinned mesh to the
  horizon.
- Rendering offscreen above the desktop's size does not work.
- Synchronization validation has never actually been run - only the standard
  validation layers.
