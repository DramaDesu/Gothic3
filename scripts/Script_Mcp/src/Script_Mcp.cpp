#include "mcp_server.h"

#include <g3sdk/Script.h>
#include <g3sdk/util/Hook.h>
#include <g3sdk/util/Memory.h>

extern GEFloat g_fAttackSpeedMultiplier;
extern mCCallHook g_HookAttackSpeed;
void GE_STDCALL ApplyAttackSpeedMultiplier(GEFloat &o_fAttackSpeed);

gSScriptInit &GetScriptInit()
{
    static gSScriptInit s_ScriptInit;
    return s_ScriptInit;
}

extern "C" __declspec(dllexport) gSScriptInit const *GE_STDCALL ScriptInit(void)
{
    // Ensure that that Script_Game.dll is loaded.
    GetScriptAdmin().LoadScriptDLL("Script_Game.dll");

    // Register McpAdmin engine component.
    static bCAccessorCreator McpAdmin(bTClassName<mCMcpAdmin>::GetUnmangled());

    // Same call site Script_AttackSpeed patches; the multiplier itself is live.
    g_HookAttackSpeed.Prepare(RVA_ScriptGame(0x4D5B), &ApplyAttackSpeedMultiplier)
        .InsertCall()
        .AddStackArg(0x10)
        .Hook();

    return &GetScriptInit();
}

//
// Entry Point
//

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID)
{
    switch (dwReason)
    {
        case DLL_PROCESS_ATTACH: ::DisableThreadLibraryCalls(hModule); break;
        case DLL_PROCESS_DETACH: break;
    }
    return TRUE;
}
