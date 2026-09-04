---
name: gothic3-live
description: Drive the real, running Gothic 3 game from this repo - launch it, get it into a world, read the player and other entities, teleport, take screenshots. Use this whenever a task needs the original game as a reference or an oracle: checking how the shipping engine behaves, what a value really is at runtime, how an animation or camera looks, or confirming that our own runtime matches. Also use it when a g3_* MCP tool times out or reports remote_control_loaded false, because the obvious reading of that is wrong and this explains why.
---

# Driving the live Gothic 3

The game is the only oracle for behaviour. Our runtime reads the same data, but
what the engine *does* with it - timings, camera, what is solid, what an entity
really holds - is answered by running the shipping game and asking it. This is
how to do that without losing an hour to the parts that are not obvious.

## The two channels, and which one works

There are two control channels into the patched game, and they are easy to
confuse because the MCP tools are named after the one that is broken here.

| | port 5555 | port 5556 |
| --- | --- | --- |
| protocol | protobuf | JSON, one object a line |
| exposed as | `g3_entity`, `g3_goto`, `g3_property`, `g3_spawn` | nothing - talk to the socket |
| works in this setup | **no**, times out | **yes** |
| answers in the menu | - | yes |

So the MCP tools that query the world time out, and everything useful goes
through 5556. `scripts/g3.py` in this skill wraps it:

```bash
python .claude/skills/gothic3-live/scripts/g3.py ping
python .claude/skills/gothic3-live/scripts/g3.py combat_state
python .claude/skills/gothic3-live/scripts/g3.py save_game --name probe
```

These MCP tools *do* work and are worth using: `g3_launch`, `g3_screenshot`,
`g3_input`, `g3_logs`, `g3_quit`.

## Two lies to ignore

**`g3_status` reports `remote_control_loaded: false` even when everything is
fine.** It looks for `Script_RemoteControl.dll`; the file on disk is called
`Script_Mcp.dll`. The flag is wrong by construction and says nothing about
whether the game is reachable. Test with a ping on 5556 instead - that is the
only answer that means anything.

**A running game is not necessarily a reachable one.** A session started by hand
loads no script DLL at all, so no channel exists. Check before trusting one:

```powershell
(Get-Process -Id <pid>).Modules | Where-Object { $_.ModuleName -like "Script_*" }
```

Nothing listed means the process cannot be driven and has to be restarted
through `g3_launch`. `g3_launch` refuses while any game runs - it returns
`already running` rather than restarting - so the old process must be closed
first. Closing someone's session can lose their progress: ask before doing it
unless they have just told you to relaunch.

## Getting into a world

The channel answers in the main menu, but almost nothing else works there. Two
routes in, and only one of them is available:

**`new_game`** works and takes about 160 seconds to reach `INGAME`. Poll for it
rather than sleeping blind:

```bash
python .claude/skills/gothic3-live/scripts/g3.py wait --timeout 300
```

**Loading a save does not work at all, by any route found so far.** There is no
`load_game` on this channel. `mcp/cycle.py` sends **F9** and calls it a
quickload, and that is worth not believing: F9 sent through `g3_input` loads
nothing, at the menu or in a world.

The two tests, so nobody has to repeat them. At the main menu, after F9 the state
was still `MENU` two minutes later. In a world, F9 left `state_time` climbing -
67 seconds before, 114 after - where a load would have reset it. It is the
absence of that reset that settles it; the position and health look identical
either way when the save was taken where the hero already stands, which is what
makes this easy to get wrong.

So the only way into a world is `new_game`, every time, at about 160 seconds.
Making a save reachable means adding a `load_game` handler to `Script_Mcp.dll` -
its sibling handlers are in
`gothic3sdk-examples/examples/Script_RemoteControl/src/handler/` and are short.
That is the single highest-value change available to this setup.

`list_saves` lists what exists and `save_game --name X` writes one, and both
work - a save is tracked in `ping`'s `last_save`. Writing one still costs
nothing and will be worth having the day loading works.

## Do not drive the menu with the mouse

It looks like it should work and it does not. The in-game cursor is not drawn
into `g3_screenshot` output, so its position cannot be seen, and `g3_input`'s
`move` is relative, so a click lands somewhere unknown. Pinning the cursor to a
corner with a large negative move first does not help either - tried, and the
menu still took no hover. Use `new_game`.

## Do not hammer the channel

The server is one connection at a time and it is inside the game's own tick.
Probing thirty unknown command names back to back **crashed the game** - the
first few answered `unknown command` harmlessly, then the process died. If the
set of commands has to be explored, do it one at a time with a pause, and expect
to lose the session anyway.

Known commands: `ping`, `list_saves`, `save_game`, `new_game`, `combat_state`,
`attack_speed`. Anything else answers `unknown command`.

## What `combat_state` gives you

Despite the name it is the general player-state read, and it is the most useful
call on the channel:

```json
{"name": "PC_Hero", "position": [87984.9, 5145.6, -10197.5],
 "ani_state": 2, "ani_phase": 5, "state_time": 53.1, "task": "PS_Normal",
 "hitpoints": 200, "stamina": 150, "current_target": "Orc_..."}
```

Position is in the world's own units - centimetres, the same as our runtime's -
so a coordinate read here can be handed straight to `g3world --camera`. That is
the main reason to reach for the game: it puts a real, correct number beside
whatever we computed.

## Where the logs are

- `g3_logs --which paru_patch` - the community patch's module list, useful for
  seeing whether the patch loaded at all.
- `g3_logs --which crash` - `Documents\gothic3\Lastlog_GE3.log`. It is a memory
  report rather than a stack, so it tells you *that* it died, not where.

## A working session, start to finish

```
g3_status                     -> running? if yes, check its Script_* modules
g3_launch  1280x720 windowed  -> only after the old one is closed
g3.py ping                    -> state MENU means the channel is up
g3.py new_game
g3.py wait                    -> about 160 s to INGAME
g3.py combat_state            -> where the hero is, what it is doing
g3_screenshot                 -> look at it
g3.py save_game --name probe  -> cheap, though nothing can load it yet
```

## Reading whether something happened

`ping` carries `last_load` and `last_save`, but they only record what the channel
itself did - a load started from the keyboard leaves `last_load` untouched, so it
cannot be used to tell whether one happened.

`combat_state`'s `state_time` can: it counts up with the world and a load resets
it. That is the reliable tell for "did the world restart", and it is how the F9
claim above was disproved.
