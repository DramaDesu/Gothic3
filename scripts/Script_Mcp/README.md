# Script_Mcp

Our own Gothic 3 script DLL: a control surface for agent-driven work on the game.
Loaded automatically by the engine from `scripts/`, no launcher patching involved.

## Transport
Line-delimited JSON over TCP on `127.0.0.1:5556`. Requests are served from
`mCMcpAdmin::Process()`, i.e. once per frame on the engine main thread — the only
place where touching engine state is safe. No zmq/protobuf dependency, unlike
`Script_RemoteControl` (which stays useful for entity/goto/spawn/property).

    {"cmd":"ping"}                          -> state MENU/INGAME, game_running, paused
    {"cmd":"list_saves"}                    -> savegame names
    {"cmd":"load_save","name":"QuickSave",
     "order":"world_first|session_first|session_only|new_game"}
    {"cmd":"combat_state"}                  -> player, or {"focus":true} / {"name":"..."}
    {"cmd":"attack_speed","value":1.5}      -> live attack-speed multiplier (0.1..10)
    {"cmd":"nearby_npcs","radius":2000}     -> combat snapshot of every NPC in range

`combat_state` reports what actually decides how a fight feels: CombatState,
AttackReason, attitude, current target/attacker, last-hit timestamp, status
effects, hitpoints/stamina/mana.

## Headless world loading
`gCSession::Start()` reaches INGAME for both `LoadGame` and `NewGame`, but the
world stays empty (bare sky, no player), and `gCWorld::LoadGameWorld()` returns
false in every context tried:

| attempt | result |
| --- | --- |
| `LoadGameWorld` from the menu | false |
| `LoadGameWorld` with a live session | false |
| `Start(LoadGame)` / `Start(NewGame)` alone | session runs, world empty |
| a save written by this very build | false — so it is not save compatibility |

Static analysis of Game.dll explained all of it, and `load_save` now walks the
same path the menu does (`gCHUDFileManager::LoadGame`, RVA 0x0A9A10):

1. read the savegame header - it names the world the save belongs to
2. `gCProject::GetCurrentProject()->AccessWorld(worldName)`
3. `gCWorld::RestartWorld(false)` - **this** is the world load; for an inactive
   world it internally does Deactivate + Activate
4. `gCWorld::LoadGameWorld(save, true)` restores the save into that live world
5. `GotoLoadedPosition(PC_Camera)`, `Start(LoadGame)`, close the menu

Why the earlier attempts failed, precisely:

- `LoadGameWorld` is not a world loader at all - it restores a save into an
  **already activated** world and bails out when the current world's file base
  name differs from the one in the header, which is every call from the menu.
- `gCSession::Start` returns immediately unless `gCSession+0xC8` (its world
  pointer) is set, and that is only ever written by `WorldActivate` - a side
  effect of activating a world. So `Start` alone can never load anything.

Loading the QuickSave straight from the menu now takes about 10 seconds and
lands INGAME with the menu closed.

Entity enumeration goes through `eCSceneAdmin`'s entity map (a derived view
reaches the protected member); `Entity::GetNPCs()` comes back empty in a live
world. Player attributes come from the script-layer `PlayerMemory` - the hero's
`gCDamageReceiver_PS` only carries placeholders (hp 1/1).

A full run against the QuickSave world reports the hero at level 63 with
10320 hp, 1004 strength, and 144 NPCs within 20000 units, including which of
them currently target `PC_Hero`.
