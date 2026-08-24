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

## Known gap: headless world loading
`gCSession::Start()` reaches INGAME for both `LoadGame` and `NewGame`, but the
world stays empty (bare sky, no player), and `gCWorld::LoadGameWorld()` returns
false in every context tried:

| attempt | result |
| --- | --- |
| `LoadGameWorld` from the menu | false |
| `LoadGameWorld` with a live session | false |
| `Start(LoadGame)` / `Start(NewGame)` alone | session runs, world empty |
| a save written by this very build | false — so it is not save compatibility |

So loading a world is neither `Start` nor `LoadGameWorld`; the GUI drives
something else, and the SDK exposes no world-loading entry point. Tracing that
needs a debugger on `gCSession::OnGameMenuClicked` (exported), not more guesses.

Driving the menu through OS input does not substitute for it either: the menu
ignores the keyboard, its cursor follows relative mouse deltas only, and
`PrintWindow` captures that cursor unreliably, so clicking blind does not land.

Until then: load a save by hand, after which every command works INGAME.
