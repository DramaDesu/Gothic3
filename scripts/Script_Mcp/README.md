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
    {"cmd":"nearby_npcs","radius":2000}     -> combat snapshot of every NPC in range

`combat_state` reports what actually decides how a fight feels: CombatState,
AttackReason, attitude, current target/attacker, last-hit timestamp, status
effects, hitpoints/stamina/mana.

## Headless world loading: solved by the engine's own quickload
`gCSession::Start()` reaches INGAME for both `LoadGame` and `NewGame`, but the
world stays empty (bare sky, no player), and `gCWorld::LoadGameWorld()` returns
false in every context tried:

| attempt | result |
| --- | --- |
| `LoadGameWorld` from the menu | false |
| `LoadGameWorld` with a live session | false |
| `Start(LoadGame)` / `Start(NewGame)` alone | session runs, world empty |
| a save written by this very build | false — so it is not save compatibility |

So loading a world is neither `Start` nor `LoadGameWorld`. What does work is the
engine's own quickload action (`gESessionKey_QuickLoad`), which runs the correct
internal path:

    new_game            # gCSession::Start(NewGame) -> INGAME (empty world, ~216 s)
    g3_input keys=[f9]  # engine quickload -> the QuickSave world really loads

Verified end to end: after F9 the player is `PC_Hero` at level 63 from the
QuickSave, standing in Varant, and screenshots show the real world. So the whole
cycle is scriptable today, without touching the menu.

Menu automation through OS input is a dead end and is not needed: the menu
ignores the keyboard, its cursor follows relative mouse deltas only, and
`PrintWindow` captures that cursor unreliably.

Still open: `nearby_npcs` returns nothing in a loaded world, so `Entity::GetNPCs()`
is the wrong source - switch to enumerating `eCSceneAdmin` entities (or the
processing-range list `Game.dll` RVA 0x3F664 that Script_ModMe hooks).
