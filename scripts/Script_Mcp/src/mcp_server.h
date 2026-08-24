#pragma once

// winsock2.h must precede anything that pulls in windows.h (the SDK headers do),
// otherwise the legacy winsock.h wins and every socket type is redefined.
#include <winsock2.h>

#include <g3sdk/Game.h>

// Engine component that owns the MCP bridge socket. Process() is pumped once per
// frame on the main thread, which is the only place engine state may be touched.
class mCMcpAdmin : public eCEngineComponentBase
{
  public:
    virtual bEResult PostInitialize(void);

  public:
    virtual bEResult PreShutdown(void);

  public:
    virtual void Process(void);

  public:
    virtual ~mCMcpAdmin(void);

  private:
    static bTPropertyObject<mCMcpAdmin, eCEngineComponentBase> ms_PropertyObjectInstance_mCMcpAdmin;

  public:
    mCMcpAdmin(void);

  private:
    mCMcpAdmin(mCMcpAdmin const &);
    mCMcpAdmin const &operator=(mCMcpAdmin const &);

  private:
    void AcceptClients();
    void ServeClient();
    bCString Dispatch(bCString const &a_Request);

  private:
    SOCKET m_ListenSocket;
    SOCKET m_ClientSocket;
    bCString m_RecvBuffer;
    GEBool m_bWsaReady;
    // A load request cannot run inside the receive loop: LoadGameWorld tears down
    // the world we are iterating. It is deferred to the start of the next frame.
    bCString m_PendingLoad;
    bCString m_PendingLoadOrder;
    bCString m_LastLoadResult;
    bCString m_PendingSave;
    bCString m_LastSaveResult;
};
