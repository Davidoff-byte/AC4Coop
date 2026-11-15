// eos_core.h - shared EOS plumbing for tools/eos_spike and tools/eos_bridge (x64; EOS SDK v1.19 has no Win32 DLL).
// Platform globals, Device-ID login, lobby by CODE attribute, P2P send/receive, relay control, NAT query,
// connection-established tracking. Header-only (static functions): include it in exactly one translation unit.
#pragma once
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include "eos_sdk.h"
#include "eos_logging.h"
#include "eos_connect.h"
#include "eos_lobby.h"
#include "eos_p2p.h"

static EOS_HPlatform g_plat = nullptr;
static EOS_ProductUserId g_me = nullptr;
static const char* kSocket = "AC4COOP";

static double NowMs() { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return c.QuadPart * 1000.0 / f.QuadPart; }
static void Pump(int ms) { const double end = NowMs() + ms; while (NowMs() < end) { EOS_Platform_Tick(g_plat); Sleep(1); } }
template <class F> static bool WaitFor(F done, int timeoutMs) { const double end = NowMs() + timeoutMs; while (!done()) { if (NowMs() > end) return false; EOS_Platform_Tick(g_plat); Sleep(1); } return true; }
static std::string Puid(EOS_ProductUserId id) { char b[EOS_PRODUCTUSERID_MAX_LENGTH + 1] = {}; int32_t n = sizeof(b); EOS_ProductUserId_ToString(id, b, &n); return b; }

static void EOS_CALL OnLog(const EOS_LogMessage* m) { printf("  [eos %s] %s\n", m->Category, m->Message); }

// ---- login (Connect, Device ID) -------------------------------------------------------------------------------
static bool Login(const char* model)
{
    EOS_HConnect con = EOS_Platform_GetConnectInterface(g_plat);
    EOS_EResult rc = EOS_EResult::EOS_NotConfigured; bool done = false;
    EOS_Connect_CreateDeviceIdOptions cd = {}; cd.ApiVersion = EOS_CONNECT_CREATEDEVICEID_API_LATEST; cd.DeviceModel = model;
    struct Ctx { EOS_EResult* rc; bool* done; } ctx{ &rc, &done };
    EOS_Connect_CreateDeviceId(con, &cd, &ctx, [](const EOS_Connect_CreateDeviceIdCallbackInfo* i) { auto c = (Ctx*)i->ClientData; *c->rc = i->ResultCode; *c->done = true; });
    if (!WaitFor([&] { return done; }, 15000)) { printf("CreateDeviceId: timeout\n"); return false; }
    printf("CreateDeviceId: %s\n", EOS_EResult_ToString(rc));
    if (rc != EOS_EResult::EOS_Success && rc != EOS_EResult::EOS_DuplicateNotAllowed) return false;

    EOS_Connect_Credentials cr = {}; cr.ApiVersion = EOS_CONNECT_CREDENTIALS_API_LATEST; cr.Type = EOS_EExternalCredentialType::EOS_ECT_DEVICEID_ACCESS_TOKEN; cr.Token = nullptr;
    EOS_Connect_UserLoginInfo ui = {}; ui.ApiVersion = EOS_CONNECT_USERLOGININFO_API_LATEST; ui.DisplayName = model;
    EOS_Connect_LoginOptions lo = {}; lo.ApiVersion = EOS_CONNECT_LOGIN_API_LATEST; lo.Credentials = &cr; lo.UserLoginInfo = &ui;
    struct LCtx { EOS_EResult rc; EOS_ContinuanceToken tok; EOS_ProductUserId id; bool done; } lc{};
    EOS_Connect_Login(con, &lo, &lc, [](const EOS_Connect_LoginCallbackInfo* i) { auto c = (LCtx*)i->ClientData; c->rc = i->ResultCode; c->tok = i->ContinuanceToken; c->id = i->LocalUserId; c->done = true; });
    if (!WaitFor([&] { return lc.done; }, 20000)) { printf("Login: timeout\n"); return false; }
    printf("Login: %s\n", EOS_EResult_ToString(lc.rc));
    if (lc.rc == EOS_EResult::EOS_InvalidUser) {
        EOS_Connect_CreateUserOptions cu = {}; cu.ApiVersion = EOS_CONNECT_CREATEUSER_API_LATEST; cu.ContinuanceToken = lc.tok;
        struct UCtx { EOS_EResult rc; EOS_ProductUserId id; bool done; } uc{};
        EOS_Connect_CreateUser(con, &cu, &uc, [](const EOS_Connect_CreateUserCallbackInfo* i) { auto c = (UCtx*)i->ClientData; c->rc = i->ResultCode; c->id = i->LocalUserId; c->done = true; });
        if (!WaitFor([&] { return uc.done; }, 20000)) { printf("CreateUser: timeout\n"); return false; }
        printf("CreateUser: %s\n", EOS_EResult_ToString(uc.rc));
        if (uc.rc != EOS_EResult::EOS_Success) return false;
        g_me = uc.id;
    } else if (lc.rc == EOS_EResult::EOS_Success) {
        g_me = lc.id;
    } else return false;
    printf("ProductUserId: %s\n", Puid(g_me).c_str());
    return true;
}

// ---- P2P ------------------------------------------------------------------------------------------------------
static EOS_P2P_SocketId Sock() { EOS_P2P_SocketId s = {}; s.ApiVersion = EOS_P2P_SOCKETID_API_LATEST; strncpy_s(s.SocketName, kSocket, _TRUNCATE); return s; }
static bool Send(EOS_ProductUserId to, uint8_t ch, const void* d, uint32_t n, EOS_EPacketReliability rel)
{
    EOS_P2P_SocketId s = Sock();
    EOS_P2P_SendPacketOptions o = {}; o.ApiVersion = EOS_P2P_SENDPACKET_API_LATEST; o.LocalUserId = g_me; o.RemoteUserId = to; o.SocketId = &s;
    o.Channel = ch; o.DataLengthBytes = n; o.Data = d; o.bAllowDelayedDelivery = EOS_TRUE; o.Reliability = rel; o.bDisableAutoAcceptConnection = EOS_FALSE;
    return EOS_P2P_SendPacket(EOS_Platform_GetP2PInterface(g_plat), &o) == EOS_EResult::EOS_Success;
}
static int Recv(EOS_ProductUserId* from, uint8_t* ch, uint8_t* buf, uint32_t cap)
{
    EOS_P2P_ReceivePacketOptions o = {}; o.ApiVersion = EOS_P2P_RECEIVEPACKET_API_LATEST; o.LocalUserId = g_me; o.MaxDataSizeBytes = cap; o.RequestedChannel = nullptr;
    EOS_P2P_SocketId s = {}; uint32_t n = 0;
    const EOS_EResult r = EOS_P2P_ReceivePacket(EOS_Platform_GetP2PInterface(g_plat), &o, from, &s, ch, buf, &n);
    return r == EOS_EResult::EOS_Success ? (int)n : -1;
}
static void SetRelay(const char* mode)
{
    EOS_P2P_SetRelayControlOptions o = {}; o.ApiVersion = EOS_P2P_SETRELAYCONTROL_API_LATEST;
    o.RelayControl = !strcmp(mode, "force") ? EOS_ERelayControl::EOS_RC_ForceRelays : !strcmp(mode, "none") ? EOS_ERelayControl::EOS_RC_NoRelays : EOS_ERelayControl::EOS_RC_AllowRelays;
    printf("SetRelayControl(%s): %s\n", mode, EOS_EResult_ToString(EOS_P2P_SetRelayControl(EOS_Platform_GetP2PInterface(g_plat), &o)));
}
// Connection-established event (eos_p2p_types.h): NetworkType says direct vs relayed. Runs 2026-11-15 (two PCs in different
// hemispheres): the first phase was sent before the link was up and lost every echo, so the guest now waits for this first.
static volatile int g_established = 0;   // 0 = none, else 1 + EOS_ENetworkConnectionType
static const char* NetName(int e) { return e == 1 + (int)EOS_ENetworkConnectionType::EOS_NCT_DirectConnection ? "DIRECT"
                                         : e == 1 + (int)EOS_ENetworkConnectionType::EOS_NCT_RelayedConnection ? "RELAYED" : "none"; }
static void WatchEstablished()
{
    EOS_P2P_SocketId s = Sock();
    EOS_P2P_AddNotifyPeerConnectionEstablishedOptions o = {}; o.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONESTABLISHED_API_LATEST; o.LocalUserId = g_me; o.SocketId = &s;
    EOS_P2P_AddNotifyPeerConnectionEstablished(EOS_Platform_GetP2PInterface(g_plat), &o, nullptr, [](const EOS_P2P_OnPeerConnectionEstablishedInfo* i) {
        g_established = 1 + (int)i->NetworkType;
        printf("connection established with %s: %s (%s)\n", Puid(i->RemoteUserId).c_str(), NetName(g_established),
               i->ConnectionType == EOS_EConnectionEstablishedType::EOS_CET_Reconnection ? "reconnection" : "new");
    });
}

static void QueryNat()
{
    EOS_P2P_QueryNATTypeOptions q = {}; q.ApiVersion = EOS_P2P_QUERYNATTYPE_API_LATEST; bool done = false;
    EOS_P2P_QueryNATType(EOS_Platform_GetP2PInterface(g_plat), &q, &done, [](const EOS_P2P_OnQueryNATTypeCompleteInfo* i) { *(bool*)i->ClientData = true; });
    WaitFor([&] { return done; }, 10000);
    EOS_P2P_GetNATTypeOptions g = {}; g.ApiVersion = EOS_P2P_GETNATTYPE_API_LATEST; EOS_ENATType t = EOS_ENATType::EOS_NAT_Unknown;
    EOS_P2P_GetNATType(EOS_Platform_GetP2PInterface(g_plat), &g, &t);
    const char* names[] = { "Unknown", "Open", "Moderate", "Strict" };
    printf("NAT type: %s\n", names[(int)t < 4 ? (int)t : 0]);
}

// ---- lobby ----------------------------------------------------------------------------------------------------
static std::string g_lobbyId;
static std::string MakeCode() { const char* a = "ACDEFGHJKLMNPQRTUVWXY34679"; std::string s; LARGE_INTEGER c; QueryPerformanceCounter(&c); uint64_t x = c.QuadPart ^ (GetCurrentProcessId() * 0x9E3779B97F4A7C15ull);
    for (int i = 0; i < 6; ++i) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; s += a[x % 26]; } return s; }

static bool HostLobby(const std::string& code)
{
    EOS_HLobby lob = EOS_Platform_GetLobbyInterface(g_plat);
    EOS_Lobby_CreateLobbyOptions o = {}; o.ApiVersion = EOS_LOBBY_CREATELOBBY_API_LATEST; o.LocalUserId = g_me; o.MaxLobbyMembers = 2;
    o.PermissionLevel = EOS_ELobbyPermissionLevel::EOS_LPL_PUBLICADVERTISED; o.bPresenceEnabled = EOS_FALSE; o.bAllowInvites = EOS_FALSE;
    o.BucketId = "ac4coop:spike"; o.bDisableHostMigration = EOS_TRUE; o.bEnableRTCRoom = EOS_FALSE; o.bEnableJoinById = EOS_FALSE;
    struct C { EOS_EResult rc; std::string id; bool done; } c{};
    EOS_Lobby_CreateLobby(lob, &o, &c, [](const EOS_Lobby_CreateLobbyCallbackInfo* i) { auto x = (C*)i->ClientData; x->rc = i->ResultCode; if (i->LobbyId) x->id = i->LobbyId; x->done = true; });
    if (!WaitFor([&] { return c.done; }, 20000)) { printf("CreateLobby: timeout\n"); return false; }
    printf("CreateLobby: %s\n", EOS_EResult_ToString(c.rc));
    if (c.rc != EOS_EResult::EOS_Success) return false;
    g_lobbyId = c.id;
    EOS_Lobby_UpdateLobbyModificationOptions mo = {}; mo.ApiVersion = EOS_LOBBY_UPDATELOBBYMODIFICATION_API_LATEST; mo.LocalUserId = g_me; mo.LobbyId = g_lobbyId.c_str();
    EOS_HLobbyModification mod = nullptr;
    if (EOS_Lobby_UpdateLobbyModification(lob, &mo, &mod) != EOS_EResult::EOS_Success) { printf("UpdateLobbyModification failed\n"); return false; }
    EOS_Lobby_AttributeData ad = {}; ad.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST; ad.Key = "CODE"; ad.Value.AsUtf8 = code.c_str(); ad.ValueType = EOS_ELobbyAttributeType::EOS_AT_STRING;
    EOS_LobbyModification_AddAttributeOptions aa = {}; aa.ApiVersion = EOS_LOBBYMODIFICATION_ADDATTRIBUTE_API_LATEST; aa.Attribute = &ad; aa.Visibility = EOS_ELobbyAttributeVisibility::EOS_LAT_PUBLIC;
    printf("AddAttribute: %s\n", EOS_EResult_ToString(EOS_LobbyModification_AddAttribute(mod, &aa)));
    EOS_Lobby_UpdateLobbyOptions uo = {}; uo.ApiVersion = EOS_LOBBY_UPDATELOBBY_API_LATEST; uo.LobbyModificationHandle = mod;
    struct U { EOS_EResult rc; bool done; } u{};
    EOS_Lobby_UpdateLobby(lob, &uo, &u, [](const EOS_Lobby_UpdateLobbyCallbackInfo* i) { auto x = (U*)i->ClientData; x->rc = i->ResultCode; x->done = true; });
    WaitFor([&] { return u.done; }, 20000);
    EOS_LobbyModification_Release(mod);
    printf("UpdateLobby: %s\n", EOS_EResult_ToString(u.rc));
    return u.rc == EOS_EResult::EOS_Success;
}

static EOS_ProductUserId JoinByCode(const std::string& code)
{
    EOS_HLobby lob = EOS_Platform_GetLobbyInterface(g_plat);
    EOS_Lobby_CreateLobbySearchOptions so = {}; so.ApiVersion = EOS_LOBBY_CREATELOBBYSEARCH_API_LATEST; so.MaxResults = 1;
    EOS_HLobbySearch search = nullptr;
    if (EOS_Lobby_CreateLobbySearch(lob, &so, &search) != EOS_EResult::EOS_Success) { printf("CreateLobbySearch failed\n"); return nullptr; }
    EOS_Lobby_AttributeData ad = {}; ad.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST; ad.Key = "CODE"; ad.Value.AsUtf8 = code.c_str(); ad.ValueType = EOS_ELobbyAttributeType::EOS_AT_STRING;
    EOS_LobbySearch_SetParameterOptions sp = {}; sp.ApiVersion = EOS_LOBBYSEARCH_SETPARAMETER_API_LATEST; sp.Parameter = &ad; sp.ComparisonOp = EOS_EComparisonOp::EOS_CO_EQUAL;
    printf("SetParameter: %s\n", EOS_EResult_ToString(EOS_LobbySearch_SetParameter(search, &sp)));
    EOS_LobbySearch_FindOptions fo = {}; fo.ApiVersion = EOS_LOBBYSEARCH_FIND_API_LATEST; fo.LocalUserId = g_me;
    struct F { EOS_EResult rc; bool done; } f{};
    EOS_LobbySearch_Find(search, &fo, &f, [](const EOS_LobbySearch_FindCallbackInfo* i) { auto x = (F*)i->ClientData; x->rc = i->ResultCode; x->done = true; });
    WaitFor([&] { return f.done; }, 20000);
    EOS_LobbySearch_GetSearchResultCountOptions gc = {}; gc.ApiVersion = EOS_LOBBYSEARCH_GETSEARCHRESULTCOUNT_API_LATEST;
    const uint32_t n = EOS_LobbySearch_GetSearchResultCount(search, &gc);
    printf("Find: %s, results=%u\n", EOS_EResult_ToString(f.rc), n);
    if (f.rc != EOS_EResult::EOS_Success || n == 0) { EOS_LobbySearch_Release(search); return nullptr; }
    EOS_LobbySearch_CopySearchResultByIndexOptions ci = {}; ci.ApiVersion = EOS_LOBBYSEARCH_COPYSEARCHRESULTBYINDEX_API_LATEST; ci.LobbyIndex = 0;
    EOS_HLobbyDetails det = nullptr; EOS_LobbySearch_CopySearchResultByIndex(search, &ci, &det);
    EOS_LobbyDetails_GetLobbyOwnerOptions go = {}; go.ApiVersion = EOS_LOBBYDETAILS_GETLOBBYOWNER_API_LATEST;
    EOS_ProductUserId owner = EOS_LobbyDetails_GetLobbyOwner(det, &go);
    EOS_Lobby_JoinLobbyOptions jo = {}; jo.ApiVersion = EOS_LOBBY_JOINLOBBY_API_LATEST; jo.LobbyDetailsHandle = det; jo.LocalUserId = g_me; jo.bPresenceEnabled = EOS_FALSE;
    struct J { EOS_EResult rc; std::string id; bool done; } j{};
    EOS_Lobby_JoinLobby(lob, &jo, &j, [](const EOS_Lobby_JoinLobbyCallbackInfo* i) { auto x = (J*)i->ClientData; x->rc = i->ResultCode; if (i->LobbyId) x->id = i->LobbyId; x->done = true; });
    WaitFor([&] { return j.done; }, 20000);
    printf("JoinLobby: %s, host=%s\n", EOS_EResult_ToString(j.rc), owner ? Puid(owner).c_str() : "?");
    if (j.rc == EOS_EResult::EOS_Success) g_lobbyId = j.id;
    EOS_LobbyDetails_Release(det); EOS_LobbySearch_Release(search);
    return j.rc == EOS_EResult::EOS_Success ? owner : nullptr;
}

static void LeaveLobby(bool host)
{
    if (g_lobbyId.empty()) return;
    EOS_HLobby lob = EOS_Platform_GetLobbyInterface(g_plat); bool done = false;
    if (host) {
        EOS_Lobby_DestroyLobbyOptions o = {}; o.ApiVersion = EOS_LOBBY_DESTROYLOBBY_API_LATEST; o.LocalUserId = g_me; o.LobbyId = g_lobbyId.c_str();
        EOS_Lobby_DestroyLobby(lob, &o, &done, [](const EOS_Lobby_DestroyLobbyCallbackInfo* i) { printf("DestroyLobby: %s\n", EOS_EResult_ToString(i->ResultCode)); *(bool*)i->ClientData = true; });
    } else {
        EOS_Lobby_LeaveLobbyOptions o = {}; o.ApiVersion = EOS_LOBBY_LEAVELOBBY_API_LATEST; o.LocalUserId = g_me; o.LobbyId = g_lobbyId.c_str();
        EOS_Lobby_LeaveLobby(lob, &o, &done, [](const EOS_Lobby_LeaveLobbyCallbackInfo* i) { printf("LeaveLobby: %s\n", EOS_EResult_ToString(i->ResultCode)); *(bool*)i->ClientData = true; });
    }
    WaitFor([&] { return done; }, 10000);
}


// ---- platform start (credentials from an ini, never printed) --------------------------------------------------
// ini: [EOS] ProductId / SandboxId / DeploymentId / ClientId / ClientSecret (local/eos.ini, git-ignored).
// tag: per-instance cache directory under local\eos-cache\<tag>. Returns false (with a message) on any failure.
static bool StartPlatform(const std::string& iniPath, const std::string& tag, const char* productName)
{
    char full[MAX_PATH]; GetFullPathNameA(iniPath.c_str(), MAX_PATH, full, nullptr);
    auto get = [&](const char* k) { char b[256] = {}; GetPrivateProfileStringA("EOS", k, "", b, sizeof(b), full); return std::string(b); };
    static std::string pid, sid, did, cid, csec;   // must outlive EOS_Platform_Create
    pid = get("ProductId"); sid = get("SandboxId"); did = get("DeploymentId"); cid = get("ClientId"); csec = get("ClientSecret");
    if (pid.empty() || sid.empty() || did.empty() || cid.empty() || csec.empty()) { printf("missing [EOS] keys in %s\n", full); return false; }
    printf("credentials loaded from %s (not printed)\n", full);
    EOS_InitializeOptions io = {}; io.ApiVersion = EOS_INITIALIZE_API_LATEST; io.ProductName = productName; io.ProductVersion = "0.1";
    const EOS_EResult r = EOS_Initialize(&io); printf("EOS_Initialize: %s\n", EOS_EResult_ToString(r));
    if (r != EOS_EResult::EOS_Success && r != EOS_EResult::EOS_AlreadyConfigured) return false;
    EOS_Logging_SetCallback(&OnLog); EOS_Logging_SetLogLevel(EOS_ELogCategory::EOS_LC_ALL_CATEGORIES, EOS_ELogLevel::EOS_LOG_Warning);
    static char cache[MAX_PATH]; GetFullPathNameA(("local\\eos-cache\\" + tag).c_str(), MAX_PATH, cache, nullptr);
    CreateDirectoryA("local", nullptr); CreateDirectoryA("local\\eos-cache", nullptr); CreateDirectoryA(cache, nullptr);
    EOS_Platform_Options po = {}; po.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST; po.ProductId = pid.c_str(); po.SandboxId = sid.c_str(); po.DeploymentId = did.c_str();
    po.ClientCredentials.ClientId = cid.c_str(); po.ClientCredentials.ClientSecret = csec.c_str(); po.bIsServer = EOS_FALSE;
    po.Flags = EOS_PF_DISABLE_OVERLAY; po.CacheDirectory = cache; po.TickBudgetInMilliseconds = 0;
    g_plat = EOS_Platform_Create(&po);
    if (!g_plat) { printf("EOS_Platform_Create failed\n"); return false; }
    return true;
}
