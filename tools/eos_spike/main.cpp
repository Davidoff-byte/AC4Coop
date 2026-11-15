// eos_spike - standalone EOS check for docs/design/eos-transport.md (x64; the EOS SDK v1.19 ships no Win32 DLL).
// host:  eos_spike host [--tag A] [--relay force|allow|none]
// join:  eos_spike join <CODE> [--tag B] [--relay ...]
// Credentials come from local/eos.ini (git-ignored) at run time; they are never printed.
// Oracle (design doc): device-ID login, lobby by code, P2P echo of 576 B / 1170 B unreliable + reliable-ordered,
// loss, order and RTT, then a clean shutdown (leave/destroy lobby, release handles).
#include "eos_core.h"

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
