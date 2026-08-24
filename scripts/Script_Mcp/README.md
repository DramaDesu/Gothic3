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
    {"cmd":"new_game"}                      -> activate G3_World_01 and start fresh
    {"cmd":"save_game","name":"arena"}      -> write a savegame
    {"cmd":"teleport","x":..,"y":..,"z":..} -> move the player (or "to":"<entity>")
    {"cmd":"spawn","template":"Orc_Warrior_01","count":3,"distance":600}
    {"cmd":"attributes"}                    -> attribute breakdown
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

## Arena notes

Template names come from `Templates.pak`, where each entry is `<Layer>_<Name>.tple`
and `SpawnEntity` wants the `<Name>` half only: `Orc_Warrior_01`..`Orc_Warrior_12`,
`Wolf`, `Warg`, `IceWolf`, `Schakal`, `AssWarrior_01`..`AssWarrior_10`.

Two engine calls look inviting and are wrong:

- `gCWorld::CreateSaveGame` is editor-side. It fails during play and leaves the
  session without a current world, which breaks saving and listing afterwards.
  Saving goes through `SaveGameWorld`, the counterpart of `LoadGameWorld`.
- Calling `gCSession::Stop()` or deactivating the world before `RestartWorld`
  kills the game with "can't remove player and camera entities because there is
  no world left" - `gCWorld::Activate` already calls `Stop()` itself.

Telemetry was checked against a known reference: a freshly started character
reports 200 hp and 100 strength/dexterity, which are Gothic 3's starting values,
so the level-63 save's 10320 hp and 1004 strength are real. The `level` field on
`gCNPC_PS` is not the player's level - it reads 63 even on a new game.
