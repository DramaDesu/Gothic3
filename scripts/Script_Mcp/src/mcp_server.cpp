#include "mcp_server.h"

#include "mcp_json.h"

#include <g3sdk/Script.h>
#include <g3sdk/util/Hook.h>
#include <g3sdk/util/Logging.h>
#include <g3sdk/util/Memory.h>
#include <g3sdk/util/Util.h>

// Combat feel knob: Script_Game computes an attack speed per swing and we scale
// it on the way out, so it can be retuned live instead of per build.
GEFloat g_fAttackSpeedMultiplier = 1.0f;
mCCallHook g_HookAttackSpeed;

void GE_STDCALL ApplyAttackSpeedMultiplier(GEFloat &o_fAttackSpeed)
{
    o_fAttackSpeed *= g_fAttackSpeedMultiplier;
}

namespace
{
GEU16 const c_uPort = 5556;
GEInt const c_iMaxRequest = 64 * 1024;

bCString Fail(GELPCChar a_pError)
{
    return mCJsonWriter().Bool("ok", GEFalse).Str("error", bCString(a_pError)).Finish();
}

// eCSceneAdmin keeps every entity in a protected map; a derived view is the
// cheapest way to walk it (Entity::GetNPCs() comes back empty in a live world).
class mCSceneEntities : public eCSceneAdmin
{
  public:
    bTPtrMap<bCPropertyID, eCEntity *> &All()
    {
        return m_mapEntities;
    }
};

eCEntity *FindEntityByName(bCString const &a_Name)
{
    eCSceneAdmin *pSceneAdmin = FindModule<eCSceneAdmin>();
    if (!pSceneAdmin)
        return 0;

    eCEntity *pEntity = pSceneAdmin->GetEntityByName(a_Name);
    if (!pEntity)
        pEntity = pSceneAdmin->GetEntityByPartName(a_Name, eEGetEntityTypeHint_Entity);
    return pEntity;
}

eCEntity *ResolveEntity(mCJsonRequest const &a_Request)
{
    if (a_Request.Has("focus"))
    {
        if (gCFocus_PS *pFocus =
                GetPropertySet<gCFocus_PS>(gCSession::GetInstance().GetPlayer(), eEPropertySetType_Focus))
            return pFocus->AccessCurrentEntity().GetEntity();
        return 0;
    }
    if (a_Request.Has("name"))
        return FindEntityByName(a_Request.GetString("name"));
    return gCSession::GetInstance().GetPlayer();
}

// Combat snapshot of one NPC: the numbers that decide how a fight feels.
bCString DescribeCombat(eCEntity *a_pEntity)
{
    mCJsonWriter Writer;
    Writer.Str("name", a_pEntity->GetName());
    Writer.Vector("position", a_pEntity->GetWorldPosition());

    if (gCNPC_PS const *pNpc = GetPropertySet<gCNPC_PS>(a_pEntity, eEPropertySetType_NPC))
    {
        Writer.Int("combat_state", static_cast<GEI32>(pNpc->GetCombatState()));
        Writer.Int("attack_reason", static_cast<GEI32>(pNpc->GetAttackReason().GetValue()));
        Writer.Int("level", static_cast<GEI32>(pNpc->GetLevel()));
        Writer.Int("species", static_cast<GEI32>(pNpc->GetSpecies().GetValue()));
        Writer.Int("attitude_to_player", static_cast<GEI32>(pNpc->GetAttitudeToPlayer2().GetValue()));
        Writer.Bool("immortal", pNpc->GetImmortal());
        Writer.Float("last_dist_to_target", pNpc->GetLastDistToTarget());
        Writer.Int("last_hit_timestamp", static_cast<GEI32>(pNpc->GetLastHitTimestamp()));
        Writer.Int("status_effects", static_cast<GEI32>(pNpc->GetStatusEffects()));

        if (eCEntity const *pTarget = pNpc->GetCurrentTargetEntity().GetEntity())
            Writer.Str("current_target", pTarget->GetName());
        if (eCEntity const *pAttacker = pNpc->GetCurrentAttackerEntity().GetEntity())
            Writer.Str("current_attacker", pAttacker->GetName());
    }

    if (GetPropertySet<gCPlayerMemory_PS>(a_pEntity, eEPropertySetType_PlayerMemory))
    {
        // The hero's real attributes live in the script-layer PlayerMemory;
        // his gCDamageReceiver_PS carries placeholder values.
        Entity Wrapper(a_pEntity);
        Writer.Int("hitpoints", Wrapper.PlayerMemory.GetHitPoints());
        Writer.Int("hitpoints_max", Wrapper.PlayerMemory.GetHitPointsMax());
        Writer.Int("stamina", Wrapper.PlayerMemory.GetStaminaPoints());
        Writer.Int("stamina_max", Wrapper.PlayerMemory.GetStaminaPointsMax());
        Writer.Int("mana", Wrapper.PlayerMemory.GetManaPoints());
        Writer.Int("mana_max", Wrapper.PlayerMemory.GetManaPointsMax());
        Writer.Int("strength", Wrapper.PlayerMemory.GetStrength());
        Writer.Int("dexterity", Wrapper.PlayerMemory.GetDexterity());
        Writer.Bool("is_player", GETrue);
    }
    else if (gCDamageReceiver_PS const *pDamage =
            GetPropertySet<gCDamageReceiver_PS>(a_pEntity, eEPropertySetType_DamageReceiver))
    {
        Writer.Int("hitpoints", pDamage->GetHitPoints());
        Writer.Int("hitpoints_max", pDamage->GetHitPointsMax());
        Writer.Int("stamina", pDamage->GetStaminaPoints());
        Writer.Int("stamina_max", pDamage->GetStaminaPointsMax());
        Writer.Int("mana", pDamage->GetManaPoints());
        Writer.Int("mana_max", pDamage->GetManaPointsMax());
    }

    return Writer.Finish();
}
} // namespace

bTPropertyObject<mCMcpAdmin, eCEngineComponentBase> mCMcpAdmin::ms_PropertyObjectInstance_mCMcpAdmin(GETrue);

mCMcpAdmin::mCMcpAdmin(void)
    : m_ListenSocket(INVALID_SOCKET), m_ClientSocket(INVALID_SOCKET), m_bWsaReady(GEFalse)
{
    PostInitialize();
    eCModuleAdmin::GetInstance().RegisterModule(*this);
}

bEResult mCMcpAdmin::PostInitialize(void)
{
    WSADATA WsaData;
    if (::WSAStartup(MAKEWORD(2, 2), &WsaData) != 0)
        return bEResult_False;
    m_bWsaReady = GETrue;

    m_ListenSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_ListenSocket == INVALID_SOCKET)
        return bEResult_False;

    BOOL bReuse = TRUE;
    ::setsockopt(m_ListenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char const *>(&bReuse), sizeof(bReuse));

    sockaddr_in Address;
    ::memset(&Address, 0, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = ::inet_addr("127.0.0.1");
    Address.sin_port = ::htons(c_uPort);

    if (::bind(m_ListenSocket, reinterpret_cast<sockaddr *>(&Address), sizeof(Address)) == SOCKET_ERROR ||
        ::listen(m_ListenSocket, 1) == SOCKET_ERROR)
    {
        ::closesocket(m_ListenSocket);
        m_ListenSocket = INVALID_SOCKET;
        return bEResult_False;
    }

    u_long bNonBlocking = 1;
    ::ioctlsocket(m_ListenSocket, FIONBIO, &bNonBlocking);
    return bEResult_Ok;
}

bEResult mCMcpAdmin::PreShutdown(void)
{
    if (m_ClientSocket != INVALID_SOCKET)
        ::closesocket(m_ClientSocket);
    if (m_ListenSocket != INVALID_SOCKET)
        ::closesocket(m_ListenSocket);
    m_ClientSocket = INVALID_SOCKET;
    m_ListenSocket = INVALID_SOCKET;
    if (m_bWsaReady)
    {
        ::WSACleanup();
        m_bWsaReady = GEFalse;
    }
    UnregisterModule(this);
    return bEResult_Ok;
}

mCMcpAdmin::~mCMcpAdmin(void)
{}

void mCMcpAdmin::Process(void)
{
    if (!m_PendingSave.IsEmpty())
    {
        bCString SaveName = m_PendingSave;
        m_PendingSave = "";

        gCWorld *pWorld = gCWorld::GetCurrentWorld();
        if (!pWorld)
            m_LastSaveResult = "no world";
        else if (!gCSession::GetInstance().IsSaveAllowed())
            m_LastSaveResult = "saving not allowed right now";
        else
        {
            eCProcessibleElement::eEResult Result = pWorld->CreateSaveGame(SaveName);
            bCString Tmp;
            Tmp.Format("CreateSaveGame -> %d", static_cast<GEInt>(Result));
            m_LastSaveResult = Tmp;
        }
    }

    if (!m_PendingLoad.IsEmpty())
    {
        bCString SaveName = m_PendingLoad;
        bCString Order = m_PendingLoadOrder;
        m_PendingLoad = "";

        gCWorld *pWorld = gCWorld::GetCurrentWorld();
        gCSession &Session = gCSession::GetInstance();
        if (!pWorld)
        {
            m_LastLoadResult = "no world";
        }
        else if (Order == "hud" || Order.IsEmpty())
        {
            // Mirrors gCHUDFileManager::LoadGame, which is what the menu runs:
            // the savegame header names the world, that world gets (re)activated
            // - RestartWorld does Deactivate+Activate for an inactive world -
            // and only then does LoadGameWorld restore the save into it.
            gCProject *pProject = gCProject::GetCurrentProject();
            if (!pProject)
            {
                m_LastLoadResult = "no current project";
            }
            else
            {
                GEU8 HeaderBuffer[0x100];
                memset(HeaderBuffer, 0, sizeof(HeaderBuffer));
                gCWorld::gCSaveGameHeader &Header = *reinterpret_cast<gCWorld::gCSaveGameHeader *>(HeaderBuffer);
                if (pWorld->GetSaveGameHeader(SaveName, Header) != bEResult_Ok)
                {
                    m_LastLoadResult = "savegame header not readable";
                }
                else
                {
                    bCString WorldName = *reinterpret_cast<bCString *>(HeaderBuffer + 0x0C);
                    gCWorld *pTarget = pProject->AccessWorld(WorldName);
                    if (!pTarget)
                    {
                        m_LastLoadResult = bCString("world not found: ") + WorldName;
                    }
                    else
                    {
                        Session.Stop();
                        pTarget->RestartWorld(GEFalse);
                        GEBool bLoaded = pTarget->LoadGameWorld(SaveName, GETrue);
                        if (!bLoaded)
                        {
                            m_LastLoadResult = bCString("LoadGameWorld failed for world ") + WorldName;
                        }
                        else
                        {
                            eCSceneAdmin *pSceneAdmin = FindModule<eCSceneAdmin>();
                            eCEntity *pCamera =
                                pSceneAdmin ? pSceneAdmin->GetEntityByName(bCString("PC_Camera")) : 0;
                            Session.GotoLoadedPosition(pCamera, GETrue);
                            Session.Start(gESession_StartMode_LoadGame);
                            // The GUI closes its menu as part of loading; we do it
                            // too so the world is actually visible afterwards.
                            if (gCGUIManager *pGui = Session.GetGUIManager())
                                pGui->CloseMenu();
                            m_LastLoadResult = bCString("loaded ") + SaveName + " in " + WorldName;
                        }
                    }
                }
            }
        }
        else if (Order == "session_first")
        {
            Session.Start(gESession_StartMode_LoadGame);
            GEBool bLoaded = pWorld->LoadGameWorld(SaveName, GETrue);
            m_LastLoadResult = bLoaded ? "session_first: loaded" : "session_first: LoadGameWorld failed";
        }
        else if (Order == "session_only")
        {
            Session.Start(gESession_StartMode_LoadGame);
            m_LastLoadResult = "session_only: started";
        }
        else if (Order == "new_game")
        {
            Session.Start(gESession_StartMode_NewGame);
            m_LastLoadResult = "new_game: started";
        }
        else
        {
            // Loading the world data is only half of it: from the main menu the
            // session still has to be started, which spawns player and camera.
            GEBool bLoaded = pWorld->LoadGameWorld(SaveName, GETrue);
            m_LastLoadResult = bLoaded ? "world_first: loaded" : "world_first: LoadGameWorld failed";
            if (bLoaded)
            {
                Session.Start(gESession_StartMode_LoadGame);
                m_LastLoadResult = "world_first: started";
            }
        }
    }

    AcceptClients();
    ServeClient();
}

void mCMcpAdmin::AcceptClients(void)
{
    if (m_ListenSocket == INVALID_SOCKET || m_ClientSocket != INVALID_SOCKET)
        return;

    SOCKET Client = ::accept(m_ListenSocket, 0, 0);
    if (Client == INVALID_SOCKET)
        return;

    u_long bNonBlocking = 1;
    ::ioctlsocket(Client, FIONBIO, &bNonBlocking);
    m_ClientSocket = Client;
    m_RecvBuffer = "";
}

void mCMcpAdmin::ServeClient(void)
{
    if (m_ClientSocket == INVALID_SOCKET)
        return;

    char Chunk[4096];
    for (;;)
    {
        int iReceived = ::recv(m_ClientSocket, Chunk, sizeof(Chunk) - 1, 0);
        if (iReceived == 0)
        {
            ::closesocket(m_ClientSocket);
            m_ClientSocket = INVALID_SOCKET;
            return;
        }
        if (iReceived < 0)
        {
            if (::WSAGetLastError() == WSAEWOULDBLOCK)
                break;
            ::closesocket(m_ClientSocket);
            m_ClientSocket = INVALID_SOCKET;
            return;
        }
        Chunk[iReceived] = 0;
        m_RecvBuffer += Chunk;
        if (m_RecvBuffer.GetLength() > c_iMaxRequest)
        {
            m_RecvBuffer = "";
            break;
        }
    }

    // Requests are newline-delimited JSON objects.
    for (;;)
    {
        GEInt iNewline = m_RecvBuffer.Find("\n");
        if (iNewline < 0)
            break;

        bCString Request = m_RecvBuffer.Left(iNewline);
        m_RecvBuffer = m_RecvBuffer.Mid(iNewline + 1);

        bCString Response = Dispatch(Request) + "\n";
        int iSent = ::send(m_ClientSocket, Response.GetText(), Response.GetLength(), 0);
        if (iSent == SOCKET_ERROR)
        {
            ::closesocket(m_ClientSocket);
            m_ClientSocket = INVALID_SOCKET;
            return;
        }
    }
}

bCString mCMcpAdmin::Dispatch(bCString const &a_RequestJson)
{
    mCJsonRequest Request(a_RequestJson);
    bCString Command = Request.GetString("cmd");

    if (Command == "ping")
    {
        gCSession &Session = gCSession::GetInstance();
        gCGUIManager const *pGui = Session.GetGUIManager();
        GEBool bMenu = pGui && pGui->IsMenuOpen();
        return mCJsonWriter()
            .Bool("ok", GETrue)
            .Str("state", bCString(bMenu ? "MENU" : "INGAME"))
            .Bool("game_running", Session.IsGameRunning())
            .Bool("paused", Session.IsPaused())
            .Str("last_load", m_LastLoadResult)
            .Str("last_save", m_LastSaveResult)
            .Finish();
    }

    if (Command == "list_saves")
    {
        gCWorld *pWorld = gCWorld::GetCurrentWorld();
        if (!pWorld)
            return Fail("no world");

        bTObjArray<bCString> arrSaves;
        pWorld->GetSaveGameList(arrSaves);

        bCString List = "[";
        for (GEInt i = 0; i < arrSaves.GetCount(); i++)
        {
            if (i)
                List += ",";
            List += "\"";
            List += mCJsonWriter::Escape(arrSaves[i]);
            List += "\"";
        }
        List += "]";
        return mCJsonWriter().Bool("ok", GETrue).Raw("saves", List).Int("count", arrSaves.GetCount()).Finish();
    }

    if (Command == "load_save")
    {
        bCString SaveName = Request.GetString("name");
        if (SaveName.IsEmpty())
            return Fail("name required");

        gCWorld *pWorld = gCWorld::GetCurrentWorld();
        if (!pWorld)
            return Fail("no world");
        if (!pWorld->ExistsSaveGame(SaveName))
            return Fail("savegame does not exist");

        // Answer before the world goes away; the load runs at the next frame start.
        m_PendingLoad = SaveName;
        m_PendingLoadOrder = Request.GetString("order", "hud");
        return mCJsonWriter().Bool("ok", GETrue).Str("loading", SaveName).Finish();
    }

    if (Command == "new_game")
    {
        m_PendingLoad = "new";
        m_PendingLoadOrder = "new_game";
        return mCJsonWriter().Bool("ok", GETrue).Str("starting", bCString("new game")).Finish();
    }

    if (Command == "save_game")
    {
        bCString SaveName = Request.GetString("name");
        if (SaveName.IsEmpty())
            return Fail("name required");
        if (!gCSession::GetInstance().IsGameRunning())
            return Fail("no running game to save");

        m_PendingSave = SaveName;
        return mCJsonWriter().Bool("ok", GETrue).Str("saving", SaveName).Finish();
    }

    if (Command == "attack_speed")
    {
        if (Request.Has("value"))
        {
            GEFloat fValue = Request.GetFloat("value", 1.0f);
            if (fValue < 0.1f || fValue > 10.0f)
                return Fail("value must be between 0.1 and 10.0");
            g_fAttackSpeedMultiplier = fValue;
        }
        return mCJsonWriter().Bool("ok", GETrue).Float("multiplier", g_fAttackSpeedMultiplier).Finish();
    }

    if (Command == "combat_state")
    {
        eCEntity *pEntity = ResolveEntity(Request);
        if (!pEntity)
            return Fail("entity not found");
        return mCJsonWriter().Bool("ok", GETrue).Raw("entity", DescribeCombat(pEntity)).Finish();
    }

    if (Command == "nearby_npcs")
    {
        eCDynamicEntity *pPlayer = gCSession::GetInstance().GetPlayer();
        if (!pPlayer)
            return Fail("no player");

        GEFloat fRadius = Request.GetFloat("radius", 2000.0f);
        bCVector const &PlayerPosition = pPlayer->GetWorldPosition();

        eCSceneAdmin *pSceneAdmin = FindModule<eCSceneAdmin>();
        if (!pSceneAdmin)
            return Fail("no scene admin");

        bTPtrMap<bCPropertyID, eCEntity *> &Entities = static_cast<mCSceneEntities *>(pSceneAdmin)->All();
        bCString List = "[";
        GEInt iEmitted = 0;
        for (bTValMap<bCPropertyID, eCEntity *>::bCIterator Iter = Entities.Begin(); Iter != Entities.End(); Iter++)
        {
            eCEntity *pEntity = *Iter;
            if (!pEntity || pEntity == pPlayer)
                continue;
            if (!GetPropertySet<gCNPC_PS>(pEntity, eEPropertySetType_NPC))
                continue;

            bCVector Delta = pEntity->GetWorldPosition() - PlayerPosition;
            GEFloat fDistance = Delta.GetMagnitude();
            if (fDistance > fRadius)
                continue;

            if (iEmitted++)
                List += ",";
            mCJsonWriter Writer;
            Writer.Raw("state", DescribeCombat(pEntity));
            Writer.Float("distance", fDistance);
            List += Writer.Finish();
        }
        List += "]";
        return mCJsonWriter().Bool("ok", GETrue).Raw("npcs", List).Int("count", iEmitted).Finish();
    }

    return Fail("unknown command");
}
