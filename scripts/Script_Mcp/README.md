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

## Known gap: headless savegame loading
`gCSession::Start(gESession_StartMode_LoadGame)` does get the session INGAME, but
`gCWorld::LoadGameWorld()` returns false both from the menu and from a live
session, so the world comes up empty. The menu's real load path has not been
traced yet — that needs a debugger on the GUI callback, not more guessing.
Until then: load a save by hand, after which every command works INGAME.
