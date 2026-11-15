// eos_spike - standalone EOS check for docs/design/eos-transport.md (x64; the EOS SDK v1.19 ships no Win32 DLL).
// host:  eos_spike host [--tag A] [--relay force|allow|none]
// join:  eos_spike join <CODE> [--tag B] [--relay ...]
// Credentials come from local/eos.ini (git-ignored) at run time; they are never printed.
// Oracle (design doc): device-ID login, lobby by code, P2P echo of 576 B / 1170 B unreliable + reliable-ordered,
// loss, order and RTT, then a clean shutdown (leave/destroy lobby, release handles).
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

// ---- echo test ------------------------------------------------------------------------------------------------
#pragma pack(push, 1)
struct Probe { uint32_t magic, phase, seq; double sentMs; };
#pragma pack(pop)
static const uint32_t kMagic = 0x45305331;   // 'E0S1'

static void HostLoop(int seconds)
{
    EOS_P2P_SocketId s = Sock();
    EOS_P2P_AddNotifyPeerConnectionRequestOptions no = {}; no.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONREQUEST_API_LATEST; no.LocalUserId = g_me; no.SocketId = &s;
    EOS_P2P_AddNotifyPeerConnectionRequest(EOS_Platform_GetP2PInterface(g_plat), &no, nullptr, [](const EOS_P2P_OnIncomingConnectionRequestInfo* i) {
        EOS_P2P_AcceptConnectionOptions ao = {}; ao.ApiVersion = EOS_P2P_ACCEPTCONNECTION_API_LATEST; ao.LocalUserId = i->LocalUserId; ao.RemoteUserId = i->RemoteUserId; ao.SocketId = i->SocketId;
        printf("connection request from %s -> accept: %s\n", Puid(i->RemoteUserId).c_str(), EOS_EResult_ToString(EOS_P2P_AcceptConnection(EOS_Platform_GetP2PInterface(g_plat), &ao)));
    });
    std::vector<uint8_t> buf(EOS_P2P_MAX_PACKET_SIZE);
    uint32_t echoed = 0; const double end = NowMs() + seconds * 1000.0;
    printf("host: echoing for %d s (Ctrl+C to stop)\n", seconds);
    while (NowMs() < end) {
        EOS_Platform_Tick(g_plat);
        for (;;) {
            EOS_ProductUserId from = nullptr; uint8_t ch = 0;
            const int n = Recv(&from, &ch, buf.data(), (uint32_t)buf.size());
            if (n < 0) break;
            if (n >= (int)sizeof(Probe) && ((Probe*)buf.data())->magic == kMagic) {
                if (((Probe*)buf.data())->phase == 99) { printf("host: guest finished, echoed %u packets (link %s)\n", echoed, NetName(g_established)); return; }
                Send(from, ch, buf.data(), (uint32_t)n, ch == 2 ? EOS_EPacketReliability::EOS_PR_ReliableOrdered : EOS_EPacketReliability::EOS_PR_UnreliableUnordered);
                ++echoed;
            }
        }
        Sleep(1);
    }
    printf("host: time up, echoed %u packets\n", echoed);
}

struct PhaseResult { const char* name; uint32_t sent, back; bool inOrder; double med, p95; };
// hz = send rate. Game-like rates (PlayerState ~60 Hz) instead of the first runs' 200/s, which queued behind a 400 ms
// intercontinental link and inflated RTT to seconds.
static PhaseResult RunPhase(EOS_ProductUserId host, uint32_t phase, const char* name, uint32_t count, uint32_t size, uint8_t ch, EOS_EPacketReliability rel, double hz)
{
    std::vector<uint8_t> out(size, 0xA5), buf(EOS_P2P_MAX_PACKET_SIZE);
    std::vector<double> rtt; std::vector<uint32_t> order;
    uint32_t sent = 0;
    const double step = 1000.0 / hz;
    double next = NowMs();
    const double deadline = NowMs() + count * step + 8000.0;
    while (NowMs() < deadline && (sent < count || rtt.size() < sent)) {
        EOS_Platform_Tick(g_plat);
        if (sent < count && NowMs() >= next) {
            Probe p{ kMagic, phase, sent, NowMs() }; memcpy(out.data(), &p, sizeof(p));
            if (Send(host, ch, out.data(), size, rel)) ++sent;
            next += step;
        }
        for (;;) {
            EOS_ProductUserId from = nullptr; uint8_t c = 0;
            const int n = Recv(&from, &c, buf.data(), (uint32_t)buf.size());
            if (n < 0) break;
            const Probe* p = (const Probe*)buf.data();
            if (n >= (int)sizeof(Probe) && p->magic == kMagic && p->phase == phase) { rtt.push_back(NowMs() - p->sentMs); order.push_back(p->seq); }
        }
        Sleep(1);
    }
    std::vector<double> s = rtt; std::sort(s.begin(), s.end());
    PhaseResult r{ name, sent, (uint32_t)rtt.size(), std::is_sorted(order.begin(), order.end()),
                   s.empty() ? 0 : s[s.size() / 2], s.empty() ? 0 : s[std::min(s.size() - 1, s.size() * 95 / 100)] };
    printf("  %-28s sent=%4u back=%4u loss=%5.1f%% order=%s rtt median=%.1f ms p95=%.1f ms\n", r.name, r.sent, r.back,
           r.sent ? 100.0 * (r.sent - r.back) / r.sent : 0.0, r.inOrder ? "kept" : "MIXED", r.med, r.p95);
    return r;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);   // visible live when piped
    if (argc < 2) { printf("usage: eos_spike host|join <CODE> [--tag X] [--relay force|allow|none] [--ini path]\n"); return 2; }
    const bool host = !strcmp(argv[1], "host");
    std::string code = (!host && argc > 2) ? argv[2] : "", tag = host ? "A" : "B", relay = "allow", ini = "local\\eos.ini";
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--tag") && i + 1 < argc) tag = argv[++i];
        else if (!strcmp(argv[i], "--relay") && i + 1 < argc) relay = argv[++i];
        else if (!strcmp(argv[i], "--ini") && i + 1 < argc) ini = argv[++i];
    }
    char full[MAX_PATH]; GetFullPathNameA(ini.c_str(), MAX_PATH, full, nullptr);
    auto get = [&](const char* k) { char b[256] = {}; GetPrivateProfileStringA("EOS", k, "", b, sizeof(b), full); return std::string(b); };
    const std::string pid = get("ProductId"), sid = get("SandboxId"), did = get("DeploymentId"), cid = get("ClientId"), csec = get("ClientSecret");
    if (pid.empty() || sid.empty() || did.empty() || cid.empty() || csec.empty()) { printf("missing keys in %s\n", full); return 2; }
    printf("eos_spike %s tag=%s relay=%s (credentials loaded from %s; not printed)\n", argv[1], tag.c_str(), relay.c_str(), full);

    EOS_InitializeOptions io = {}; io.ApiVersion = EOS_INITIALIZE_API_LATEST; io.ProductName = "AC4Coop EOS spike"; io.ProductVersion = "0.1";
    EOS_EResult r = EOS_Initialize(&io); printf("EOS_Initialize: %s\n", EOS_EResult_ToString(r));
    if (r != EOS_EResult::EOS_Success && r != EOS_EResult::EOS_AlreadyConfigured) return 1;
    EOS_Logging_SetCallback(&OnLog); EOS_Logging_SetLogLevel(EOS_ELogCategory::EOS_LC_ALL_CATEGORIES, EOS_ELogLevel::EOS_LOG_Warning);

    char cache[MAX_PATH]; GetFullPathNameA(("local\\eos-cache\\" + tag).c_str(), MAX_PATH, cache, nullptr);
    CreateDirectoryA("local\\eos-cache", nullptr); CreateDirectoryA(cache, nullptr);
    EOS_Platform_Options po = {}; po.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST; po.ProductId = pid.c_str(); po.SandboxId = sid.c_str(); po.DeploymentId = did.c_str();
    po.ClientCredentials.ClientId = cid.c_str(); po.ClientCredentials.ClientSecret = csec.c_str(); po.bIsServer = EOS_FALSE;
    po.Flags = EOS_PF_DISABLE_OVERLAY; po.CacheDirectory = cache; po.TickBudgetInMilliseconds = 0;
    g_plat = EOS_Platform_Create(&po);
    if (!g_plat) { printf("EOS_Platform_Create failed\n"); return 1; }
    printf("platform created (cache %s)\n", cache);

    int rc = 1;
    if (Login(("ac4coop-spike-" + tag).c_str())) {
        SetRelay(relay.c_str());
        WatchEstablished();
        QueryNat();
        if (host) {
            code = MakeCode();
            if (HostLobby(code)) { printf("\n*** LOBBY CODE: %s ***\n\n", code.c_str()); HostLoop(600); rc = 0; }
            LeaveLobby(true);
        } else {
            EOS_ProductUserId h = JoinByCode(code);
            if (h && Puid(h) == Puid(g_me)) {
                // 2026-11-15 run: both copies on one Windows user got the same ProductUserId (Device ID credentials are per
                // OS user profile, not per CacheDirectory), and P2P "to the host" was delivered locally with 0.0 ms RTT.
                printf("\nVERDICT: INVALID - host and guest are the same EOS user (%s). Run the join on another Windows user or PC.\n", Puid(h).c_str());
                rc = 4;
            } else if (h) {
                // Open the link with a reliable hello and wait for the established event before measuring.
                Probe hello{ kMagic, 0, 0, NowMs() };
                Send(h, 2, &hello, sizeof(hello), EOS_EPacketReliability::EOS_PR_ReliableOrdered);
                const bool up = WaitFor([] { return g_established != 0; }, 30000);
                if (!up) { printf("\nVERDICT: FAIL - no P2P connection established within 30 s\n"); rc = 3; }
                else {
                    Pump(2000);   // let the first echoes of the hello drain
                    printf("guest: link is %s; running echo phases at game-like rates\n", NetName(g_established));
                    std::vector<PhaseResult> res;
                    res.push_back(RunPhase(h, 1, "unreliable 576 B @60 Hz", 600, 576, 1, EOS_EPacketReliability::EOS_PR_UnreliableUnordered, 60.0));
                    res.push_back(RunPhase(h, 2, "unreliable 1170 B @30 Hz", 300, 1170, 1, EOS_EPacketReliability::EOS_PR_UnreliableUnordered, 30.0));
                    res.push_back(RunPhase(h, 3, "reliable-ordered 576 B @30 Hz", 300, 576, 2, EOS_EPacketReliability::EOS_PR_ReliableOrdered, 30.0));
                    Probe done{ kMagic, 99, 0, NowMs() }; Send(h, 2, &done, sizeof(done), EOS_EPacketReliability::EOS_PR_ReliableOrdered); Pump(1500);
                    const bool pass = res[2].back == res[2].sent && res[2].inOrder && res[0].sent && (res[0].sent - res[0].back) * 50 < res[0].sent;
                    printf("\nVERDICT: %s - link %s; reliable all back in order + unreliable loss < 2 %% (RTT is reported, not judged)\n",
                           pass ? "PASS" : "FAIL", NetName(g_established));
                    rc = pass ? 0 : 3;
                }
            }
            LeaveLobby(false);
        }
    }
    EOS_Platform_Release(g_plat); EOS_Shutdown();
    printf("shutdown done\n");
    return rc;
}
