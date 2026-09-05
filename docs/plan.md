# The plan

## What this is for

A greatly improved Gothic 3, built as a hybrid: the game's core and content
stay, its systems are replaced with good ones. The game shipped unfinished -
the systems were never resolved and the content has holes - and the owner's
judgment, which this plan takes as its premise, is that all of it closes with
quality systems rather than with a new game.

So the split is fixed:

- **Content is data, and we read it.** The world (2177 sectors), its people
  (35437 entities in `.lrentdat`, 10279 of them NPCs, 7651 with a daily
  routine), the dialogues and quests (`Infos.pak`, `Quests.pak`,
  `Strings.pak`), the actors, clips, materials, collision. None of this is
  rewritten. Where the content has holes, the fix is more data, authored the
  same way.
- **Systems are ours, and we write them.** Movement, combat, AI, streaming,
  rendering, animation, physics. These are where the game fell short, and
  where the runtime exists to be better. Reimplementing a system is not
  reimplementing the game: the rules are ours, the world they run in is
  Gothic's.

The original game is the **oracle**, not the target. The hook track
(`scripts/Script_Mcp`, the `gothic3-live` skill) can put the shipping game at
any coordinate and read what it does - where the hero is, what he is doing,
how hard an orc hits and how often. That is the specification a replacement
system is measured against before it is allowed to be different. "Improved"
is only a claim once "the same" has been shown.

The runtime is 64-bit, Vulkan, multithreaded, and reads the game's data
directly with no game process behind it. That was chosen over hooking the
original because the original is 32-bit, DX9 and single-ticked, and none of
the three improvement axes fit inside that.

Two things about it are the owner's, stated plainly so the plan cannot drift
from them:

- **It is his version, kept and tuned for years.** Not a patch, not a mod, not
  a port to be handed off - a codebase he owns and keeps turning. Every choice
  here favours what can be tuned later over what is finished sooner.
- **It is high-performance as a goal, not as a side effect.** Performance is
  the first-rank requirement alongside the improved systems, and the reason the
  original could not be the base. Systems are designed for scale from their
  first line - data laid out for the cache, work laid out for the pool, nothing
  per-object that could be per-batch - and measured on every step. The engine
  is also the owner's vehicle for learning multithreading, cache behaviour and
  memory, which is the same thing said from the other side.

## Where the three axes stand

**Streaming** is the axis where the runtime is probably already better than
the game: a loader on its own thread, GPU arenas with refcounted geometry, no
queue waits on upload, a parallel cull at 0.4 ms. *Probably*, because the
audit found `--fly` has been dead since the controller rewrite, so every
flight benchmark since Sep 4 measured a stationary camera. The axis is built
and unproven.

**Combat** is the long pole. Nothing of it exists in the runtime, and it needs
a game layer under it - a player with animation state, NPCs that act, damage.
It is also the axis with the best specification: the hook track has measured
the original (`docs/combat-model.md` - 37 damage a hit, 0.53 s minimum
interval, the action machine 1-35, `AssessHit` as the lever, stun-lock as the
known defect).

**Rendering** reproduces the original's look with normal maps and specular
added. Its improvements - PBR, ray tracing, DLSS - are deferred by the owner's
own decision until there is a game to put them in.

## Performance, as a requirement

The number that matters is not the frame time of the viewer today; it is what
the runtime can hold while staying under budget. Targets are stated here as
what the plan aims at, to be measured rather than assumed - every one is a
claim until a phase's "done" shows it:

- **A town at full life.** Its people all present and animated - the ones in
  view at full cost, the rest ticking their routines without being drawn - at
  a frame well under the budget on the owner's card. Today 96 bind-posed
  people cost 3 ms, two thirds of it the GPU, and the CPU posing is a quarter
  of that; both scale badly and both have named fixes in phase 2.
- **The whole world resident in memory, and streamed geometry never a
  hitch.** The world is 6.8 by 6.4 km; the original stutters on it and the
  community patch had to rewrite its file system to help. The runtime already
  holds the full world at 3.58 GB peak and loads a sector off the main
  thread; the unproven part is the moving-camera tail (phase 1).
- **Every core busy where the work allows.** The pool is there and the cull
  uses it at 4.6x. Simulation - NPC routines, animation sampling, collision
  queries - is laid out so it can go the same way: state as arrays, work as
  batches, no shared mutable maps in the tick.
- **A query budget, not a query count.** Collision at 3 us a ground query and
  a few instances a capsule is what lets a hundred bodies ask every frame.
  The budget is held by keeping the indexes honest, not by asking less.

What performance does *not* mean here: chasing the viewer's frame time below
1 ms, or optimising a path before it has a consumer. The static world path is
already at 0.44 ms for 1.45 M triangles and is left alone.

## The phases

Ordered by what the goal needs, not by what is most broken. Each phase names
what "done" looks like, because the project's recorded failures have all been
claims that outran their evidence.

### 0. Make the instruments honest - about a week

The audit (`docs/audit-2026-09.md`) found seven cheap things, and every one is
an instrument or a lie in the record rather than a feature. They come first
because every later phase measures with these:

- the loader counters that are undefined behaviour from the loader thread
  (`g3world.cpp:1136`, referenced by a `std::function` at `:483`, called at
  `:3025` after the block closed at `:1812`);
- `--fly`, so a camera can move;
- the collision rebuild booked into the input phase;
- the hero's clips, which never loop;
- the tunnelling guard, which lets 50 cm a frame through a 35 cm radius;
- the collision file the entity names, which the loader ignores for about half
  of all hulls;
- the three doc lies and the parser that accepts any flag silently.

Done: each instrument shown working on one real case before its number is
written anywhere. That is now a rule, and it is the third time it has had to
be.

### 1. Prove streaming on a moving camera - days

With `--fly` back: the per-arrival cost including the cold BVH build for
meshes never seen this session (`physics/world.cpp:285-318`, single-threaded,
on the main thread), memory growth over a long flight, and whether the tail is
real. Then, and only if the number says so, trees built on the loader thread
and instances tagged by sector so only the grid rebuilds.

Done: a flight command line committed beside its numbers, and a claim the
runtime streams better than the original that names what was measured in
both.

### 2. The game layer - weeks

This is where the hybrid starts being a game rather than a viewer.

**The controller becomes a type.** `CharacterController::step(const
CollisionWorld&, const Input&, float dt)` in `src/physics` or a new
`src/game`, with a Vulkan-free test executable against synthetic meshes and
the measured walks as byte-for-byte regressions (3991 of 5250 on the slope,
1741/1884/1735 in the wood, 496 into a trunk). This is the first seam with a
second consumer - the test - and where the first year of gameplay lives.

**A multi-heading walk harness**, because sliding alongside things has never
been measured and it is the next thing a player feels.

**NPCs live their routines.** The 7651 daily routines are already read; an NPC
walking between its `WorkingPoint` and `RelaxingPoint` on the hero's
controller, playing its own idle and walk found by name in the animation
archive, is the first living world. NPC state as a struct of arrays -
position, actor, clip, time, routine - from the start: the first honest
ECS-shaped data the project has, and the first natural job for the worker
pool.

**The character path made to scale first**, in the order the audit measured:
a per-piece frustum test, device-local buffers, shared textures, then the CPU
posing. The GPU wall arrives before the CPU one - 3.1 / 21.3 / 31.1 ms at 96
/ 400 / 1000 pieces.

**A tick that is not the viewer's frame.** The game loop - input, controller,
NPC routines, animation state - separated from rendering, so it can be
stepped headless, tested, and eventually run on its own thread against a
render thread. This is the point at which `g3world.cpp` stops being where
gameplay lives, and the point at which the multithreaded shape of the engine
is decided by a real tick rather than by a diagram.

Done: a town with its people going about their day, drawn at a frame time
that is measured with the rebuild booked honestly, and a headless test that
walks the hero and one NPC through a routine with no window open.

### 3. Combat, the same and then better - months

Reproduce the original first. The action machine, hit timing, damage and the
stun behaviour are all measured on the hook track; the runtime's combat is
done when it produces those numbers against the same orc. Only then does it
change - and what changes is the owner's list, not this document's guess.

The first projectile arrives here, and with it the second consumer of
`CollisionWorld`: a per-instance group and mask so an arrow hits the canopy
the walker ignores. That is when the three collision-source lambdas leave
`main()` and become a module, and when the `solid` flag leaves the render
batch. Not before.

Done: the shipping game and the runtime, same save, same orc, same numbers -
then a documented difference that the owner asked for.

### 4. The systemic problems - the owner's list

The owner knows the game's problems and this document does not presume to.
What the community and the hook track have recorded - stun-lock, AI that does
not use what it has, faction and balance systems left half-built, the emptier
last third of the world - is a starting point for that list, not the list.
Each item lands as a system in phase 2's game layer, measured against the
original where the original has behaviour to measure.

### 5. Rendering improvements - when there is a game in them

PBR, ray tracing, DLSS, in the order the pinned reading suggests. Deferred by
choice. The renderer's static path is already sound and measured and is not
to be disturbed for these until they are due.

## Architecture, and what pulls it

No architecture pass. Extractions happen when a consumer pulls them, and the
audit was explicit that today each subsystem has one consumer: the controller
extraction is due because its test is waiting; the collision module is due
when the projectile needs a second query set; the loader gets a named state
when a headless streaming test needs one. The ECS shape comes in through the
data - NPC state as arrays in phase 2 - rather than through a framework. The
loader split in `849f95f` is the model: a measured 200 ms stall demanded it.

## What we do not do

- **Fidelity for its own sake.** The original is an oracle for systems that
  should match first. A system that has matched is free to differ.
- **Rewrite gameplay from decompilation.** The systems are ours; the content
  that would need decompiling is data instead, and we read the data.
- **Take a physics engine.** None reads this data, and the parsing was the
  work. Revisit when rigid bodies are wanted.
- **Port to another engine.** Decided on Aug 30 and not reopened.
- **Close an item on a check that cannot disagree.** See phase 0.

## The record

`docs/backlog.md` holds the open items behind these phases;
`docs/audit-2026-09.md` holds the audit that ordered phase 0; the per-subsystem
docs (`streaming.md`, `collision.md`, `world.md`, `lighting.md`,
`combat-model.md`, `genome-formats.md`) hold what was measured. A number in
this plan that is not in one of those is a claim, and should be treated as one.
