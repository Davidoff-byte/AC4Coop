// Host-IP peer-to-peer UDP link (Winsock).
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <process.h>      // _beginthreadex: RecvThread calls CRT (dbg -> printf/fputs); see dllmain.cpp "_beginthreadex, not CreateThread"
#include "net.h"
#include "paths.h"
#include "lobby_code.h"
#include "upnp.h"
#include "stun.h"
#include "relay_proto.h"

#pragma comment(lib, "ws2_32.lib")

namespace {
    bool               g_enabled = false;
    bool               g_secondInstance = false;   // LocalPort taken -> this is the same-PC 2nd instance
    SOCKET             g_sock = INVALID_SOCKET;
    sockaddr_in        g_peer{};
    CRITICAL_SECTION   g_lock;
    PlayerState        g_remote{};
    bool               g_haveRemote = false;
    EntityPacket       g_remoteEntities{};
    bool               g_haveEntities = false;
    uint64_t           g_entitiesMs = 0;      // GetTickCount64() of the last write to g_remoteEntities
    const uint64_t     NET_ENTITIES_TTL_MS = 3000;   // a snapshot older than this is from a dead peer
    bool               g_strictPeer = true;   // reject datagrams from any source but the configured peer
    uint64_t           g_lastRecvMs = 0;      // GetTickCount64() of the last ACCEPTED packet (0 = none yet)

    // ---- lobby mode (slice 3, docs/design/lobby-code.md) -------------------------------------------
    // Manual mode: g_peer comes from PeerIP and g_peerReady is 1 from the start (today's behaviour).
    // Host: g_peer is unknown until the first HELLO; join: g_peer comes from the code but the link is
    // not up until WELCOME. While g_peerReady is 0, Send/SendEntities send nothing and RecvThread
    // accepts only lobby packets. In lobby mode g_peer can be rewritten by the recv thread (host
    // re-lock) while the game thread sends, so every read and write of it goes through g_peerLock.
    // LM_LOBBY: [Net] Mode=lobby - bound and idle until the player picks Host or Join in the lobby panel
    // (Net::StartHosting / Net::JoinByCode), which then switches g_mode once to LM_HOST or LM_JOIN. Until
    // then every lobby/relay/game datagram is ignored (the handlers test for LM_HOST / LM_JOIN exactly).
    enum LobbyMode { LM_MANUAL = 0, LM_HOST = 1, LM_JOIN = 2, LM_LOBBY = 3 };
    volatile LobbyMode g_mode = LM_MANUAL;
    SRWLOCK            g_peerLock = SRWLOCK_INIT;
    volatile LONG      g_peerReady = 1;
    uint32_t           g_joinNonce = 0;       // join: this attempt's nonce (WELCOME must echo it)
    uint32_t           g_joinCheck = 0;       // join: check of the typed code (sent in HELLO, logged by the host)
    volatile LONG      g_joinState = 0;       // join: 0 idle, 1 sending HELLO, 2 welcomed, 3 rejected, 4 timed out
    volatile LONG      g_joinGen = 0;         // join attempt generation (F8 restarts)
    uint32_t           g_lockedNonce = 0;     // host: nonce of the HELLO that locked the current guest (recv thread)
    char               g_joinCodeText[16] = {0};   // the code of the current attempt, for the status line

    // ---- host: UPnP + STUN (slices 4-5) ------------------------------------------------------------
    // The STUN request goes out of the GAME socket (so the NAT mapping it reports is the one the guest
    // must reach); the recv thread recognises the answer and parks it here for the host thread.
    uint8_t            g_stunTid[12] = {0};
    volatile LONG      g_stunPending = 0;     // 1 while a request with g_stunTid is outstanding
    volatile LONG      g_stunDone = 0;        // 1 once g_stunIp/g_stunPort hold an answer
    uint32_t           g_stunIp = 0;          // host order
    uint16_t           g_stunPort = 0;
    int                g_bindPort = 0;        // the port the game socket is bound to (host thread input)
    char               g_stunServer[128] = {0};
    int                g_upnpEnabled = 1;
    char               g_hostCode[16] = {0};  // best code to share (internet if we have one, else LAN); "" = none yet
    const char* volatile g_hostNote = "";     // host thread's advice when it found no code at all (static string)
    volatile LONG      g_hostDone = 0;        // 1 once the host thread reached its verdict
    SRWLOCK            g_codeLock = SRWLOCK_INIT;

    // ---- relay (docs/design/relay.md) ----------------------------------------------------------------
    // A relayed peer's address IS the relay server's endpoint, plus a slot on that server (g_peerSlot,
    // under g_peerLock with g_peer). Everything sent to the server endpoint goes out wrapped in a DATA
    // header (SendRaw); every DATA from the server is unwrapped at the top of RecvThread before any other
    // check, so the handshake, the strict-peer gate and the decoders see the same datagrams as on a direct
    // link. With [Net] RelayServer empty (the default) none of this runs.
    char               g_relayName[128] = {0};   // [Net] RelayServer as written
    sockaddr_in        g_relaySrv{};             // resolved off the game thread (RelayResolve); 0 until then
    volatile LONG      g_relayResolved = 0;      // 1 ok, -1 failed
    int                g_relayForce = 0;         // [Net] RelayForce=1: skip the direct attempt
    uint16_t           g_peerSlot = 0;           // slot of g_peer on the relay (meaningful when g_peer == g_relaySrv)
    volatile LONG64    g_relayRoom = 0;          // host: from REGISTERED; guest: from the room code
    uint32_t           g_relayNonce = 0;         // REGISTER / JOIN nonce
    volatile LONG      g_relayRegistered = 0;    // host: REGISTERED seen
    volatile LONG      g_linkVia = 0;            // 0 none, 1 direct, 2 relay
    volatile LONG      g_relayErr = 0;           // last relay ERR code (guest status line)
    // guest: the host's endpoints from PEER (written by the recv thread before g_hostInfo = 1)
    sockaddr_in        g_hostPub{}, g_hostLan{};
    volatile LONG      g_hostInfo = 0;

    // ---- received DEATH ring (B-4: a one-shot death packet must not be lost to the snapshot slot) --
    // g_remoteEntities is last-writer-wins: `g_remoteEntities = decoded` below overwrites whatever the
    // previous datagram carried. The snapshot stream runs at SnapshotHz, so a one-entry KIND_KILL packet
    // that arrives just before a snapshot was overwritten BEFORE the main thread ever called
    // Net::GetEntities - the host had logged "[KT] death push", the guest logged nothing at all. Death
    // entries (kind==3) are therefore copied into this small ring as they are decoded, and the frame
    // drains it through GetKillEntities(), so the death is seen at least once regardless of how many
    // snapshots follow. Same idea as the player-packet ring above (FIX 4), sized far smaller because a
    // death is rare. A kill-only packet is also kept OUT of g_remoteEntities so it cannot displace a
    // snapshot (and cannot be processed twice).
    #define NET_KILLQ 32
    #define NET_KILL_DROP_LOGS 8      // log only the first N ring overflows (a burst must not spam)
    const uint8_t      NET_KIND_KILL = EK_KILL;   // phase1.cpp:2379 KIND_KILL; value declared in netstate.h
    EntityState        g_killRing[NET_KILLQ];
    int                g_killHead = 0;        // next write slot
    int                g_killCount = 0;       // filled entries; full -> the OLDEST is overwritten

    // ---- received player-packet ring (FIX 4: drain every queued packet per frame) ---------------
    // g_remote is last-wins: when two player packets land between two frames the older one is lost,
    // which starves the interpolation buffer exactly when the frame rate dips. This ring keeps them
    // all, in arrival order. g_remote/g_haveRemote are still maintained so other callers see no change.
    #define NET_RXQ 16
    PlayerState        g_rxq[NET_RXQ];
    int                g_rxqHead = 0;    // next write slot
    int                g_rxqCount = 0;   // filled entries (<= NET_RXQ); full -> the OLDEST is overwritten

    void dbg(const char* s) {
        printf("%s", s);
        OutputDebugStringA(s);
        static FILE* f = nullptr;
        if (!f) f = _fsopen(Paths::LogPath("net_log.txt"), "a", _SH_DENYWR);   // next to the ASI, not the CWD
        if (f) { fputs(s, f); fflush(f); }
    }

    // ---- same-PC instance slot marker (D-018) ----------------------------------------------------
    // A named mutex is an OWNED marker: whoever creates it first is slot 1, a second process that
    // finds it already existing is slot 2. Unlike a bind failure this cannot be defeated by
    // SO_REUSEADDR, which (deliberately, see the bind comment below) lets any number of sockets
    // bind the same port, so bind() never fails and the old "LocalPort taken" signal was dead.
    HANDLE             g_slotMutex = NULL;   // kept open for the process lifetime; closing it releases the marker

    bool DetectSecondInstance() {
        static bool s_done   = false;
        static bool s_second = false;
        if (!s_done) {
            s_done = true;
            SetLastError(0);
            HANDLE h = CreateMutexW(NULL, FALSE, L"AC4Coop_InstanceSlot_v1");
            if (!h) {
                char m[96]; wsprintfA(m, "[Net] instance marker failed err=%lu (assuming slot 1)\n", GetLastError()); dbg(m);
                s_second = false;
            } else {
                s_second = (GetLastError() == ERROR_ALREADY_EXISTS);
                g_slotMutex = h;   // never closed, not even on shutdown: it IS the marker
            }
        }
        return s_second;
    }

    volatile LONG g_txPackets = 0, g_txBytes = 0, g_txEnts = 0, g_rxPackets = 0, g_rxBytes = 0;
    // Rejects: datagrams from the peer that arrived but contributed NO state (version mismatch, a
    // failed decode, or an illegal entity kind). They still count in rxPackets/rxBytes - they really
    // were on the wire - but they do NOT refresh g_lastRecvMs (see the liveness note in RecvThread).
    volatile LONG g_rxRejects = 0;        // dropped datagrams, EXCLUDING DEC_PENDING and DEC_STALE chunks
    volatile LONG g_rxProtoMismatch = 0;  // subset of the above: protoVer != NET_PROTO_VER on either lane
    volatile LONG g_rxSizeMismatch = 0;   // player lane only: magic matched but n != sizeof(PlayerState)
    volatile LONG g_rxStaleChunks = 0;    // entity lane: DEC_STALE, a late chunk of a superseded snapshot.
                                          // The drop is correct, but it is NOT a reject - a benign
                                          // reorder is normal on a well-formed link - so it is excluded
                                          // from g_rxRejects exactly like DEC_PENDING.

    // ---- Phase 2: compact entity wire ---------------------------------------------------------
    // A record carries a stable 64-bit identity. `key` is the entity-manager INDEX and is NOT stable
    // (the list shifts as NPCs stream in/out), so keying a delta shadow on it both kills the savings
    // and can apply a delta to the wrong entity. objId is stable; where there is none (ships) we
    // synthesise an id with bit 63 set, which can never collide with a real objId.
    struct ShadowSlot { uint64_t id; EntityState st; };
    static const int SHADOW_SLOTS = 4096;
    ShadowSlot g_txShadow[SHADOW_SLOTS];
    ShadowSlot g_rxShadow[SHADOW_SLOTS];
    uint8_t    g_txBuf[16384];
    uint32_t   g_txEpoch = 1;
    uint32_t   g_txSinceReset = 0;
    uint32_t   g_rxEpoch = 1;
    static const uint32_t EPOCH_PACKETS = 64;    // both sides drop their shadow this often; also the
                                                 // self-heal bound after a lost delta packet (~3 s)
    uint32_t   g_wireFullRecs = 0;               // instrumentation: records that carried every field
    uint32_t   g_wireRecs = 0;                   // instrumentation: records in the last packet

    // ---- X15 (chunked transport) + X5 (world identity on the entity lane) ----------------------
    // A snapshot whose encoded record region would exceed ENTITY_CHUNK_MAX (1400 B UDP payload) is
    // split into chunkTotal datagrams, each <= 1400 B, so no chunk IP-fragments at MTU 1500 (one lost
    // fragment no longer discards the whole snapshot). The header repeats in every chunk.
    static const int ENTITY_CHUNK_MAX       = 1400;                              // UDP payload budget
    static const int ENTITY_CHUNK_DATA      = ENTITY_CHUNK_MAX - ENTITY_WIRE_HEADER_SIZE; // 1368 B records/chunk
    static const int ENTITY_CHUNK_MAX_TOTAL = 12;                                // = ceil((16384-32)/1368): the
                                                                                 // receive buffer cannot hold more
    static const uint64_t ENTITY_CHUNK_TIMEOUT_MS = 1000;   // drop an incomplete set after 1 s

    // Local world id, set by phase1.cpp (World::GetObjectID()) and read by BOTH the encoder (main
    // thread) and the decoder (recv thread). Interlocked 64-bit because this is an x86 build, where a
    // plain u64 read can tear across its two 32-bit halves.
    volatile LONGLONG g_localWorldId = 0;
    inline uint64_t ReadLocalWorldId() {
        return (uint64_t)InterlockedCompareExchange64(&g_localWorldId, 0, 0);
    }

    // X5 bootstrap allowance (m4), shared by the decoder (recv thread) and SetLocalWorldId (main
    // thread). 1 = a non-zero world match has been seen since the last world change. Reset by
    // SetLocalWorldId on an actual world change so the new world's worldId == 0 bootstrap frame is
    // tolerated again instead of being DEC_WORLD forever.
    volatile LONG g_worldConfirmed = 0;

    // Receiver reassembly: one in-flight snapshot (seq is monotonic; a new seq discards the old one).
    struct RxAssembly {
        uint32_t seq;
        uint32_t epoch;
        uint16_t chunkTotal;
        uint16_t receivedMask;   // bit i set once chunk i has arrived
        uint32_t receivedBytes;  // record bytes buffered so far
        uint64_t firstMs;        // first chunk arrival (bounded drop)
        bool     active;
        uint8_t  data[16384];    // header + record region (reassembled)
    };
    RxAssembly g_asm;

    // M1 hardening: the mask has one bit per chunk, and the receive buffer must hold the largest set
    // the encoder can ever emit. recordBytes <= sizeof(g_txBuf) - header = 16352, and
    // ceil(16352/1368) = 12 chunks (the last one short): 11*1368 = 15048 < 16352, 15*1368 = 20520 does not.
    static_assert(ENTITY_CHUNK_MAX_TOTAL <= 16,
                  "receivedMask is uint16_t: one bit per chunk index");
    static_assert((ENTITY_CHUNK_MAX_TOTAL - 1) * ENTITY_CHUNK_DATA
                      < (int)sizeof(g_asm.data) - ENTITY_WIRE_HEADER_SIZE,
                  "a full chunk set does not fit RxAssembly::data");
    static_assert((int)((sizeof(g_txBuf) - ENTITY_WIRE_HEADER_SIZE + ENTITY_CHUNK_DATA - 1)
                        / ENTITY_CHUNK_DATA) <= ENTITY_CHUNK_MAX_TOTAL,
                  "encoder can emit more chunks than the validator accepts");

    // RFC 1982 serial-number comparison on the uint32 seq counter: valid while |a-b| < 2^31, i.e. for
    // any reordering distance a real link can produce.
    static inline int32_t SeqDiff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

    inline uint64_t ShadowId(uint8_t kind, uint32_t key, uint64_t objId) {
        if (objId) return objId;
        return 0x8000000000000000ull | ((uint64_t)(kind & 3) << 32) | (uint64_t)key;
    }
    inline uint32_t ShadowHash(uint64_t id) {
        uint64_t x = id * 0x9E3779B97F4A7C15ull;
        return (uint32_t)(x >> 52) & (SHADOW_SLOTS - 1);
    }
    // Open addressing, bounded probe. Returns nullptr only if the table is saturated (impossible at
    // <=1792 ids per epoch); the caller then sends a full record, which is always correct.
    ShadowSlot* ShadowFind(ShadowSlot* table, uint64_t id, bool create) {
        const uint32_t h = ShadowHash(id);
        for (uint32_t i = 0; i < 64; ++i) {
            ShadowSlot& s = table[(h + i) & (SHADOW_SLOTS - 1)];
            if (s.id == id) return &s;
            if (s.id == 0) {
                if (!create) return nullptr;
                s.id = id;
                std::memset(&s.st, 0, sizeof(s.st));
                return &s;
            }
        }
        return nullptr;
    }

    size_t EncodeEntityPacket(const EntityPacket& p, uint8_t* out, size_t cap) {
        if (cap < sizeof(EntityWireHeader)) return 0;
        // Symmetric epoch reset: the receiver drops its base when it sees a new epoch, so the two
        // tables can never drift apart.
        if (++g_txSinceReset > EPOCH_PACKETS) {
            std::memset(g_txShadow, 0, sizeof(g_txShadow));
            g_txSinceReset = 0;
            ++g_txEpoch;
        }
        EntityWireHeader h{};
        h.magic = p.magic; h.protoVer = p.protoVer; h.seq = p.seq; h.epoch = g_txEpoch;
        h.worldId = ReadLocalWorldId();      // X5: sender's world id rides the header
        h.count = 0;                         // patched to the WRITTEN count after the loop
        std::memcpy(out, &h, sizeof(h));
        size_t o = sizeof(h);
        uint32_t fullRecs = 0;
        uint32_t written = 0;
        for (uint32_t i = 0; i < p.count && i < (uint32_t)MAX_SYNC_ENTITIES; ++i) {
            const EntityState& e = p.ents[i];
            const uint64_t id = ShadowId(e.kind, e.key, e.objId);
            ShadowSlot* sh = ShadowFind(g_txShadow, id, true);
            uint16_t mask = 0;
            if (!sh) {
                mask = (uint16_t)(EF_ALL & ~EF_OBJID);   // table saturated: send everything
            } else {
                // Quantised delta: position/quaternion only go on the wire when they moved enough to be
                // visible. The shadow keeps the SENT value, so small movements accumulate rather than
                // being lost. 5 cm / ~0.3 deg.
                const float dx = e.pos[0] - sh->st.pos[0];
                const float dy = e.pos[1] - sh->st.pos[1];
                const float dz = e.pos[2] - sh->st.pos[2];
                if (dx * dx + dy * dy + dz * dz > 0.0025f) mask |= EF_POS;
                float dq = 0.0f;
                for (int q = 0; q < 4; ++q) { float d = e.quat[q] - sh->st.quat[q]; dq += (d < 0.0f ? -d : d); }
                if (dq > 0.005f) mask |= EF_QUAT;
                if (e.actionAlias != sh->st.actionAlias) mask |= EF_ACTION;
                if (e.hp          != sh->st.hp)          mask |= EF_HP;
                if (e.arch        != sh->st.arch)        mask |= EF_ARCH;
                // C2 §2.1: the dead bit is a LEVEL, not a delta. A corpse whose other fields have
                // stopped changing would otherwise be omitted entirely (mask == 0) and its death
                // would survive only through the epoch re-baseline. `|| e.dead != 0` re-sends the bit
                // on every snapshot while dead; the `!=` term still fires 1 -> 0 on resurrection,
                // which is the only way a body may come back.
                // WIRE: this changes FREQUENCY, not layout - no new field, no new kind, no size
                // change, NET_PROTO_VER stays 11. An unmodified peer decodes a repeated
                // EF_DEAD = 1 and re-applies it idempotently.
                if (e.dead != sh->st.dead || e.dead != 0u) mask |= EF_DEAD;
                if (e.stage       != sh->st.stage)       mask |= EF_STAGE;
                // objId is carried by the record identity, never by a field bit.
            }
            // Nothing changed since the last packet: omit the record entirely (0 bytes instead of a
            // 16-byte stub). Absence is safe - ApplyGuards/ApplyShips only drive what is present.
            if (mask == 0) continue;
            const uint8_t idKind = (e.objId ? 0 : 1);   // 0 = real objId, 1 = synthesised from kind+key
            const uint64_t wireId = e.objId ? e.objId
                                            : (((uint64_t)(e.kind & 3) << 32) | (uint64_t)e.key);
            // Bounds: the fixed part is 16 B and the optional field bytes add up to 42 more, so the
            // old check (fixed 16 only) let a record near the end of the buffer write up to 42 bytes
            // past `cap`. Compute the REAL record size from the mask before writing anything.
            size_t need = 16;
            if (mask & EF_POS)    need += 12;
            if (mask & EF_QUAT)   need += 16;
            if (mask & EF_ACTION) need += 4;
            if (mask & EF_HP)     need += 2;
            if (mask & EF_ARCH)   need += 2;
            if (mask & EF_DEAD)   need += 1;
            if (mask & EF_STAGE)  need += 1;
            if (o + need > cap) break;   // full: send the records that fit; h.count is patched to `written` below
            std::memcpy(out + o, &e.key, 4); o += 4;
            out[o++] = e.kind;
            out[o++] = idKind;
            std::memcpy(out + o, &mask, 2); o += 2;
            std::memcpy(out + o, &wireId, 8); o += 8;
            if (mask & EF_POS)    { std::memcpy(out + o, e.pos, 12); o += 12; }
            if (mask & EF_QUAT)   { std::memcpy(out + o, e.quat, 16); o += 16; }
            if (mask & EF_ACTION) { std::memcpy(out + o, &e.actionAlias, 4); o += 4; }
            if (mask & EF_HP)     { std::memcpy(out + o, &e.hp, 2); o += 2; }
            if (mask & EF_ARCH)   { std::memcpy(out + o, &e.arch, 2); o += 2; }
            if (mask & EF_DEAD)   { out[o++] = e.dead; }
            if (mask & EF_STAGE)  { out[o++] = e.stage; }
            if (sh) {
                sh->st.key = e.key; sh->st.kind = e.kind; sh->st.objId = e.objId;
                if (mask & EF_POS)    std::memcpy(sh->st.pos,  e.pos,  sizeof(e.pos));
                if (mask & EF_QUAT)   std::memcpy(sh->st.quat, e.quat, sizeof(e.quat));
                if (mask & EF_ACTION) sh->st.actionAlias = e.actionAlias;
                if (mask & EF_HP)     sh->st.hp = e.hp;
                if (mask & EF_ARCH)   sh->st.arch = e.arch;
                if (mask & EF_DEAD)   sh->st.dead = e.dead;
                if (mask & EF_STAGE)  sh->st.stage = e.stage;
            }
            if (mask == (uint16_t)(EF_ALL & ~EF_OBJID)) ++fullRecs;
            ++written;
        }
        h.count = written;                   // the receiver writes exactly this many records
        std::memcpy(out, &h, sizeof(h));
        g_wireRecs = written;
        g_wireFullRecs = fullRecs;
        return o;
    }

    // Why a datagram was dropped, so the recv thread can COUNT and LOG the reason instead of leaving
    // a mis-versioned or malformed peer looking healthy (rxPackets used to climb for every drop).
    enum DecodeReason : uint8_t {
        DEC_OK = 0,    // decoded
        DEC_SHORT,     // shorter than the wire header
        DEC_MAGIC,     // wrong NET_ENTITY_MAGIC
        DEC_PROTO,     // protoVer != NET_PROTO_VER
        DEC_COUNT,     // count > MAX_SYNC_ENTITIES
        DEC_TRUNC,     // a record, or one of its fields, runs past the datagram
        DEC_MASK,      // a field bit outside the legal wire mask (EF_ALL & ~EF_OBJID)
        DEC_KIND,      // entity kind not in {1 ship, 2 guard, 3 kill} (see EntityKind)
        DEC_WORLD,     // header worldId != local world id (world-transitions X5)
        DEC_PENDING,   // chunk buffered for reassembly; snapshot not complete yet (X15)
        DEC_STALE,     // chunk for a snapshot OLDER than the one being reassembled: dropped (M1)
    };

    // Oracle for the Phase 2 codec, run at startup on both peers:
    bool DecodeEntityPacket(const uint8_t* in, size_t len, EntityPacket& out,
                            DecodeReason* why = nullptr);   // fwd decl

    static bool SameEntity(const EntityState& a, const EntityState& b) {
        return a.key == b.key && a.kind == b.kind && a.dead == b.dead && a.stage == b.stage
            && a.pos[0] == b.pos[0] && a.pos[1] == b.pos[1] && a.pos[2] == b.pos[2]
            && a.quat[0] == b.quat[0] && a.quat[1] == b.quat[1]
            && a.quat[2] == b.quat[2] && a.quat[3] == b.quat[3]
            && a.actionAlias == b.actionAlias
            && a.objId == b.objId && a.hp == b.hp && a.arch == b.arch;
    }

    bool SelfTestWire() {
        EntityPacket a{};
        a.magic = NET_ENTITY_MAGIC; a.protoVer = NET_PROTO_VER; a.seq = 1; a.count = 3;
        for (int i = 0; i < 3; ++i) {
            EntityState& e = a.ents[i];
            e.key = (uint32_t)i; e.kind = 2; e.dead = (uint8_t)(i == 1); e.stage = (uint8_t)(i + 1);
            e.pos[0] = 10.5f + i; e.pos[1] = -3.25f * i; e.pos[2] = 7.0f;
            e.quat[0] = 0.1f; e.quat[1] = 0.2f; e.quat[2] = 0.3f; e.quat[3] = 0.9f;
            e.actionAlias = 0x11223344u + i;
            e.objId = 0xAABBCCDD00000000ull + i; e.arch = (uint16_t)(0x35F7 + i);
            // C2 §4.1: `dead` and `hp` are one value. A dead body carries the 0xFFFF sentinel, so the
            // fixture is what a cross-check-normalised sender emits: never dead=1 with a live hp.
            e.hp = (i == 1) ? 0xFFFFu : (uint16_t)(50 + i);
        }
        std::memset(g_txShadow, 0, sizeof(g_txShadow));
        std::memset(g_rxShadow, 0, sizeof(g_rxShadow));
        g_txEpoch = g_rxEpoch = 1; g_txSinceReset = 0;
        uint8_t buf[16384];
        const size_t n1 = EncodeEntityPacket(a, buf, sizeof(buf));
        EntityPacket b{};
        bool ok = n1 > 0 && DecodeEntityPacket(buf, n1, b) && b.count == 3;
        if (ok) for (int i = 0; i < 3; ++i) ok = ok && SameEntity(a.ents[i], b.ents[i]);
        // C2 §2.4 (2): second pass, nothing changed. The DEAD body must still be re-sent - the dead
        // bit is a level, not a delta - while the two live bodies must still be omitted.
        const size_t n2 = EncodeEntityPacket(a, buf, sizeof(buf));
        EntityPacket c{};
        bool ok2 = n2 > sizeof(EntityWireHeader) && DecodeEntityPacket(buf, n2, c)
                   && c.count == 1 && c.ents[0].key == 1u && c.ents[0].dead == 1;
        // C2 §2.4 (3): third pass, resurrection. `dead` falls back to 0 and hp must come back off the
        // 0xFFFF sentinel with it, which is what the cross-check requires of a live body. Exactly one
        // record, EF_DEAD set on the wire, decoded dead == 0.
        a.ents[1].dead = 0; a.ents[1].hp = 40;
        const size_t n3 = EncodeEntityPacket(a, buf, sizeof(buf));
        EntityPacket d{};
        bool ok3 = n3 > sizeof(EntityWireHeader) && DecodeEntityPacket(buf, n3, d)
                   && d.count == 1 && d.ents[0].key == 1u && d.ents[0].dead == 0
                   && d.ents[0].hp == 40;
        // C2 §2.4 (1): the no-change omission assertion STAYS, for live entities. This is the only
        // test protecting snapshot size; it is re-fixtured, never deleted, to make §2 pass.
        const size_t n4 = EncodeEntityPacket(a, buf, sizeof(buf));
        EntityPacket f{};
        bool ok4 = (n4 == sizeof(EntityWireHeader)) && DecodeEntityPacket(buf, n4, f) && f.count == 0;
        // C2-n1: a genuinely HALF-DEAD record - dead = 1 with a live hp (50). At HEAD the codec's own
        // encoder does NOT normalise (net.cpp:272 writes e.hp verbatim); the dead=>sentinel step lives
        // in the capture path (phase1.cpp EmitGuardEntity) and the DECODER is the backstop
        // (net.cpp:444-445). So this shape can be emitted and must come back dead=1, hp=0xFFFF.
        // Scratch packets are reused: sizeof(EntityPacket) is ~93 KB and five of them are already here.
        EntityPacket& half = f;
        EntityPacket& halfOut = d;
        half = EntityPacket{};
        half.magic = NET_ENTITY_MAGIC; half.protoVer = NET_PROTO_VER; half.seq = 2; half.count = 1;
        half.ents[0].key = 7; half.ents[0].kind = 2; half.ents[0].dead = 1; half.ents[0].hp = 50;
        half.ents[0].stage = 1; half.ents[0].arch = 0x35F7;
        const size_t n5 = EncodeEntityPacket(half, buf, sizeof(buf));
        bool ok5 = n5 > sizeof(EntityWireHeader) && DecodeEntityPacket(buf, n5, halfOut)
                   && halfOut.count == 1 && halfOut.ents[0].key == 7u && halfOut.ents[0].dead == 1
                   && halfOut.ents[0].hp == 0xFFFFu;
        // Both peers must start from an empty shadow.
        std::memset(g_txShadow, 0, sizeof(g_txShadow));
        std::memset(g_rxShadow, 0, sizeof(g_rxShadow));
        g_txEpoch = g_rxEpoch = 1; g_txSinceReset = 0;
        char m[256];
        wsprintfA(m, "[Net] wire codec selftest: roundtrip=%s nochange=%s deaddelta=%s resurrect=%s halfdead=%s"
                     " (full=%u B, repeat=%u B, res=%u B, idle=%u B, half=%u B)\n",
                  ok ? "PASS" : "FAIL", ok4 ? "PASS" : "FAIL", ok2 ? "PASS" : "FAIL",
                  ok3 ? "PASS" : "FAIL", ok5 ? "PASS" : "FAIL",
                  (unsigned)n1, (unsigned)n2, (unsigned)n3, (unsigned)n4, (unsigned)n5);
        dbg(m);
        return ok && ok2 && ok3 && ok4 && ok5;
    }

    bool DecodeEntityPacket(const uint8_t* in, size_t len, EntityPacket& out, DecodeReason* why) {
        // Records the drop reason before every `return false`, so one call site can count + log it.
        auto fail = [why](DecodeReason r) -> bool { if (why) *why = r; return false; };
        if (why) *why = DEC_OK;
        if (len < sizeof(EntityWireHeader)) return fail(DEC_SHORT);
        EntityWireHeader h{};
        std::memcpy(&h, in, sizeof(h));
        if (h.magic != NET_ENTITY_MAGIC) return fail(DEC_MAGIC);
        if (h.protoVer != NET_PROTO_VER) return fail(DEC_PROTO);
        if (h.count > (uint32_t)MAX_SYNC_ENTITIES) return fail(DEC_COUNT);
        // X5: drop snapshots whose world differs from the local one. A worldId of 0 means "unknown
        // yet" (first frame / world not loaded on the sender) and is tolerated ONLY while this
        // session has never confirmed a world match - the bootstrap allowance. After the first
        // successful resolution (sender and local agree on a non-zero world) a sender that still
        // reports 0 cannot prove it is looking at the same world as us, so it is DEC_WORLD. m4.
        {
            const uint64_t localWorld = ReadLocalWorldId();
            const bool worldConfirmed = (InterlockedCompareExchange(&g_worldConfirmed, 0, 0) != 0);
            if (localWorld != 0 && h.worldId != 0 && h.worldId != localWorld) return fail(DEC_WORLD);
            if (localWorld != 0 && h.worldId == 0 && worldConfirmed) return fail(DEC_WORLD);
            if (!worldConfirmed && localWorld != 0 && h.worldId == localWorld) {
                InterlockedExchange(&g_worldConfirmed, 1);   // one-shot: the bootstrap allowance ends here
                dbg("[Net] X5 world rule: bootstrap allowance ended - sender worldId=0 is now DEC_WORLD\n");
            }
        }
        // Same epoch rule as the sender: a new epoch means the base is gone on both sides.
        if (h.epoch != g_rxEpoch) {
            std::memset(g_rxShadow, 0, sizeof(g_rxShadow));
            g_rxEpoch = h.epoch;
        }
        out.magic = h.magic; out.protoVer = h.protoVer; out.seq = h.seq; out.count = 0;
        size_t o = sizeof(h);
        for (uint32_t i = 0; i < h.count; ++i) {
            if (o + 16 > len) return fail(DEC_TRUNC);
            uint32_t key = 0; uint8_t kind = 0; uint8_t idKind = 0; uint16_t mask = 0; uint64_t wireId = 0;
            std::memcpy(&key, in + o, 4); o += 4;
            kind = in[o++];
            idKind = in[o++];
            std::memcpy(&mask, in + o, 2); o += 2;
            std::memcpy(&wireId, in + o, 8); o += 8;
            if (mask & ~(uint16_t)(EF_ALL & ~EF_OBJID)) return fail(DEC_MASK);
            // Kind whitelist (X13): 0 is reserved and any value outside the enum is meaningless to
            // ApplyShips/ApplyGuards/ProcessKillEvents. Publishing it only moves the failure
            // downstream with the raw byte intact, so reject the datagram here and count it.
            if (kind != EK_SHIP && kind != EK_GUARD && kind != EK_KILL) return fail(DEC_KIND);
            EntityState e{};
            e.key = key; e.kind = kind; e._pad = 0;
            e.objId = (idKind == 0) ? wireId : 0;
            ShadowSlot* sh = ShadowFind(g_rxShadow, ShadowId(kind, key, e.objId), true);
            if (sh) e = sh->st, e.key = key, e.kind = kind, e._pad = 0, e.objId = (idKind == 0) ? wireId : 0;
            if (mask & EF_POS)    { if (o + 12 > len) return fail(DEC_TRUNC); std::memcpy(e.pos, in + o, 12); o += 12; }
            if (mask & EF_QUAT)   { if (o + 16 > len) return fail(DEC_TRUNC); std::memcpy(e.quat, in + o, 16); o += 16; }
            if (mask & EF_ACTION) { if (o + 4 > len) return fail(DEC_TRUNC); std::memcpy(&e.actionAlias, in + o, 4); o += 4; }
            if (mask & EF_HP)     { if (o + 2 > len) return fail(DEC_TRUNC); std::memcpy(&e.hp, in + o, 2); o += 2; }
            if (mask & EF_ARCH)   { if (o + 2 > len) return fail(DEC_TRUNC); std::memcpy(&e.arch, in + o, 2); o += 2; }
            if (mask & EF_DEAD)   { if (o + 1 > len) return fail(DEC_TRUNC); e.dead = in[o++]; }
            if (mask & EF_STAGE)  { if (o + 1 > len) return fail(DEC_TRUNC); e.stage = in[o++]; }
            // C2 §4.2 cross-check: one record may not present two answers for one fact. Only a
            // record that actually carried one of the two death fields is normalised - an untouched
            // shadow value must not ratchet, or a shadow of dead=1 would be re-derived as dead for
            // ever and a delta that legitimately omits `dead` could never be trusted. Kill-lane
            // records are exempt: hp is their payload and dead is unused (I1: kind 3 && hp == 0xFFFF,
            // one entry per packet), so the rule is gated on kind not being a kill kind.
            if (kind != EK_KILL && (mask & (EF_DEAD | EF_HP)) && (e.dead || e.hp == 0xFFFFu)) {
                e.dead = 1; e.hp = 0xFFFFu;
            }
            // The writeback below stores the normalised value, so the receiver's shadow agrees with
            // what the apply path will see and a sender-side inconsistency is not re-amplified into
            // every later delta.
            if (sh) sh->st = e;
            out.ents[out.count++] = e;
        }
        return true;
    }

    // X15: consume one entity datagram (a chunk). Returns:
    //   DEC_OK      - a complete snapshot was decoded into `out` (single chunk, or the final chunk).
    //   DEC_PENDING - the chunk was buffered; the snapshot is not complete yet (never applied).
    //   any other   - the datagram was rejected (malformed / wrong version / wrong world / bad kind).
    DecodeReason DecodeEntityChunk(const uint8_t* in, size_t len, EntityPacket& out, DecodeReason* why) {
        auto fail = [why](DecodeReason r) -> DecodeReason { if (why) *why = r; return r; };
        if (why) *why = DEC_OK;
        if (len < sizeof(EntityWireHeader)) return fail(DEC_SHORT);
        EntityWireHeader h{};
        std::memcpy(&h, in, sizeof(h));
        if (h.magic != NET_ENTITY_MAGIC) return fail(DEC_MAGIC);
        if (h.protoVer != NET_PROTO_VER) return fail(DEC_PROTO);
        if (h.chunkTotal == 0 || h.chunkTotal > ENTITY_CHUNK_MAX_TOTAL) return fail(DEC_MASK);
        if (h.chunkIndex >= h.chunkTotal) return fail(DEC_MASK);
        if (h.count > (uint32_t)MAX_SYNC_ENTITIES) return fail(DEC_COUNT);

        const size_t hdr = sizeof(h);
        const size_t chunkBytes = len - hdr;

        // Fast path: a single-chunk snapshot (the common case) decodes straight out of the datagram.
        if (h.chunkTotal == 1) {
            if (!DecodeEntityPacket(in, len, out, why)) return *why;   // reason already set
            return DEC_OK;
        }

        // Multi-chunk: reassemble. A chunk belonging to a NEWER snapshot, or a set that has aged out,
        // discards the old. A chunk belonging to an OLDER snapshot (late / reordered) is DROPPED so it
        // cannot destroy an in-progress newer set: with no retransmit, losing one already-arrived chunk
        // makes a snapshot unreassemblable forever (M1). seq is a uint32 counter, so direction is a
        // signed difference - (int32_t)(a - b) > 0 is "a newer than b" and is correct across the 2^32
        // wrap (RFC 1982 serial-number comparison; valid for any |a-b| < 2^31).
        const uint64_t now = GetTickCount64();
        if (g_asm.active) {
            const int32_t dseq = SeqDiff(h.seq, g_asm.seq);
            if (dseq < 0) return fail(DEC_STALE);           // late chunk of a superseded snapshot
            if (dseq > 0 || (now - g_asm.firstMs) > ENTITY_CHUNK_TIMEOUT_MS) {
                std::memset(&g_asm, 0, sizeof(g_asm));
            }
        }
        if (!g_asm.active) {
            g_asm.active = true;
            g_asm.seq = h.seq;
            g_asm.epoch = h.epoch;
            g_asm.chunkTotal = h.chunkTotal;
            g_asm.receivedMask = 0;
            g_asm.receivedBytes = 0;
            g_asm.firstMs = now;
            std::memcpy(g_asm.data, in, hdr);   // chunk 0's header is the canonical one
        } else if (h.epoch != g_asm.epoch || h.chunkTotal != g_asm.chunkTotal) {
            // Same seq but an inconsistent split: malformed. Drop the whole set rather than keeping a
            // partially-filled assembly that would otherwise survive until the 1 s timeout (M1 review).
            std::memset(&g_asm, 0, sizeof(g_asm));
            return fail(DEC_MASK);              // same seq but inconsistent split -> malformed
        }
        // Place this chunk's record bytes (chunks are fixed-size except the last); a duplicate chunk
        // is ignored so the byte count is never double-counted.
        if (!(g_asm.receivedMask & (uint16_t)(1u << h.chunkIndex))) {
            const size_t off = (size_t)h.chunkIndex * ENTITY_CHUNK_DATA;
            if (off + chunkBytes > sizeof(g_asm.data) - hdr) return fail(DEC_TRUNC);
            std::memcpy(g_asm.data + hdr + off, in + hdr, chunkBytes);
            g_asm.receivedMask |= (uint16_t)(1u << h.chunkIndex);
            g_asm.receivedBytes += (uint32_t)chunkBytes;
        }

        if (g_asm.receivedMask == (uint16_t)((1u << h.chunkTotal) - 1)) {
            // Complete: decode the reassembled snapshot, then consume the assembler.
            const size_t total = hdr + g_asm.receivedBytes;
            const bool ok = DecodeEntityPacket(g_asm.data, total, out, why);
            std::memset(&g_asm, 0, sizeof(g_asm));
            if (!ok) return *why;
            return DEC_OK;
        }
        return DEC_PENDING;
    }

    // ---- reject observability (X4/X18) -----------------------------------------------------------
    // A version mismatch or a malformed packet used to be dropped with no trace: rxPackets climbed,
    // so a mis-versioned peer looked alive while every one of its snapshots was discarded. One line
    // per process per lane - a mismatched peer sends at SnapshotHz, so per-packet logging would flood
    // net_log.txt - plus counters (g_rxRejects / g_rxProtoMismatch) that Net::GetStats exposes.
    void LogProtoMismatch(const char* lane, uint32_t got) {
        static bool s_player = false, s_entity = false;
        bool& logged = (lane[0] == 'p') ? s_player : s_entity;
        if (logged) return;
        logged = true;
        char m[160];
        wsprintfA(m, "[Net] %s packet REJECTED protoVer=%u expected=%u (both peers must run the same build)\n",
                  lane, (unsigned)got, (unsigned)NET_PROTO_VER);
        dbg(m);
        { char e[96]; wsprintfA(e, "[E] ev=proto-mismatch lane=%s got=%u expected=%u\n", lane, (unsigned)got, (unsigned)NET_PROTO_VER); dbg(e); }
    }
    // Player lane, anonymous-vs-named: a peer built against a different PlayerState sends a datagram
    // whose leading magic is OUR magic but whose length is not sizeof(PlayerState). playerPkt is
    // false for it, so the version branch below never runs - the datagram used to vanish without a
    // single line in net_log.txt AND without touching g_rxRejects (the outer gate only lets
    // playerPkt / entityPkt in). Name the cause once per process per lane; protoVer sits in the
    // shared prefix at +4 and is only printed when the datagram is long enough to hold it.
    void LogSizeMismatch(int n, bool haveProto, uint32_t got) {
        static bool s_logged = false;
        if (s_logged) return;
        s_logged = true;
        char m[160];
        if (haveProto)
            wsprintfA(m, "[Net] player packet SIZE MISMATCH n=%d expected=%d (peer build differs; protoVer=%u)\n",
                      n, (int)sizeof(PlayerState), (unsigned)got);
        else
            wsprintfA(m, "[Net] player packet SIZE MISMATCH n=%d expected=%d (peer build differs; protoVer=?)\n",
                      n, (int)sizeof(PlayerState));
        dbg(m);
        { char e[96]; wsprintfA(e, "[E] ev=size-mismatch n=%d expected=%d\n", n, (int)sizeof(PlayerState)); dbg(e); }
    }
    void LogKindRejected() {
        static bool s_logged = false;
        if (s_logged) return;
        s_logged = true;
        dbg("[Net] entity packet REJECTED: kind outside {1 ship, 2 guard, 3 kill} (netstate.h EntityKind)\n");
        dbg("[E] ev=reject lane=entity reason=kind\n");
    }
    void LogWorldMismatch() {
        static bool s_logged = false;
        if (s_logged) return;
        s_logged = true;
        dbg("[Net] entity packet REJECTED: header worldId != local world (world change in progress?)\n");
        dbg("[E] ev=reject lane=entity reason=world-mismatch\n");
    }
    void LogRejectSummary(LONG rejects) {
        char m[224];
        wsprintfA(m, "[Net] rx rejects=%ld protoMismatch=%ld sizeMismatch=%ld stale=%ld (datagrams dropped without applying state)\n",
                  rejects, (LONG)InterlockedCompareExchange(&g_rxProtoMismatch, 0, 0),
                  (LONG)InterlockedCompareExchange(&g_rxSizeMismatch, 0, 0),
                  (LONG)InterlockedCompareExchange(&g_rxStaleChunks, 0, 0));
        dbg(m);
    }

    // ---- lobby helpers (slice 3) ------------------------------------------------------------------
    sockaddr_in PeerCopy(uint16_t* slot = nullptr) {
        AcquireSRWLockShared(&g_peerLock);
        sockaddr_in p = g_peer;
        if (slot) *slot = g_peerSlot;
        ReleaseSRWLockShared(&g_peerLock);
        return p;
    }
    void PeerSet(const sockaddr_in& p, uint16_t slot = Relay::kHostSlot) {
        AcquireSRWLockExclusive(&g_peerLock);
        g_peer = p;
        g_peerSlot = slot;
        ReleaseSRWLockExclusive(&g_peerLock);
    }
    bool SameAddr(const sockaddr_in& a, const sockaddr_in& b) {
        return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
    }
    bool IsRelay(const sockaddr_in& a) {
        return InterlockedCompareExchange(&g_relayResolved, 0, 0) == 1 && SameAddr(a, g_relaySrv);
    }
    void FmtAddr(const sockaddr_in& a, char* out, size_t n) {
        char ip[32] = {0};
        inet_ntop(AF_INET, (void*)&a.sin_addr, ip, sizeof(ip));
        _snprintf(out, n, "%s:%u", ip, (unsigned)ntohs(a.sin_port));
        out[n - 1] = 0;
    }
    // A relay control message (or DATA) to the relay server: 16-byte header + payload.
    void RelaySend(uint8_t type, uint16_t slot, uint64_t room, const void* payload, int n) {
        uint8_t b[Relay::kMaxDatagram];
        if (n < 0 || n + (int)sizeof(Relay::Hdr) > (int)sizeof(b)) return;
        Relay::Hdr h{ Relay::kMagic, Relay::kVersion, type, slot, room };
        std::memcpy(b, &h, sizeof(h));
        if (n) std::memcpy(b + sizeof(h), payload, (size_t)n);
        sendto(g_sock, (const char*)b, (int)sizeof(h) + n, 0, (const sockaddr*)&g_relaySrv, sizeof(g_relaySrv));
    }
    // The one send path for game and lobby datagrams: direct, or wrapped in DATA when `to` is the relay.
    void SendRaw(const void* buf, int n, const sockaddr_in& to, uint16_t slot) {
        if (IsRelay(to)) RelaySend(Relay::T_DATA, slot, (uint64_t)InterlockedCompareExchange64(&g_relayRoom, 0, 0), buf, n);
        else sendto(g_sock, (const char*)buf, n, 0, (const sockaddr*)&to, sizeof(to));
    }
    void SendLobby(uint32_t type, uint32_t nonce, uint32_t check, const sockaddr_in& to, uint16_t slot = Relay::kHostSlot) {
        LobbyPacket p{ NET_LOBBY_MAGIC, NET_PROTO_VER, type, nonce, check };
        SendRaw(&p, (int)sizeof(p), to, slot);
    }
    // The 12-bit check of the code for ip:port (the same CRC the code itself carries).
    uint32_t CodeCheckOf(uint32_t ipHost, uint16_t port) {
        const uint8_t b[6] = { (uint8_t)(ipHost >> 24), (uint8_t)(ipHost >> 16), (uint8_t)(ipHost >> 8),
                               (uint8_t)ipHost, (uint8_t)(port >> 8), (uint8_t)port };
        return LobbyCode::Crc16CcittFalse(b, 6) & 0x0FFFu;
    }

    // One lobby datagram (exactly sizeof(LobbyPacket), magic already checked). Recv thread only.
    // `slot`: the relay slot the datagram came from when `from` is the relay server (else kHostSlot).
    void HandleLobby(const LobbyPacket& p, const sockaddr_in& from, uint16_t slot) {
        const bool viaRelay = IsRelay(from);
        char fa[64]; FmtAddr(from, fa, sizeof(fa));
        if (viaRelay) { size_t l = strlen(fa); _snprintf(fa + l, sizeof(fa) - l, " (relay slot %u)", (unsigned)slot); fa[sizeof(fa) - 1] = 0; }
        char m[192];
        if (g_mode == LM_HOST && p.type == LOBBY_HELLO) {
            if (p.protoVer != NET_PROTO_VER) {
                SendLobby(LOBBY_REJECT, p.nonce, 0, from, slot);
                static int s_logs = 0;
                if (s_logs++ < 4) {
                    _snprintf(m, sizeof(m), "[Lobby] HELLO from %s refused: guest proto=%u, host proto=%u\n",
                              fa, (unsigned)p.protoVer, (unsigned)NET_PROTO_VER);
                    m[sizeof(m) - 1] = 0; dbg(m);
                }
                return;
            }
            uint16_t curSlot = 0;
            const sockaddr_in cur = PeerCopy(&curSlot);
            const bool ready = InterlockedCompareExchange(&g_peerReady, 0, 0) != 0;
            const bool sameGuest = SameAddr(cur, from) && (!viaRelay || curSlot == slot);
            if (ready && sameGuest) {   // a retry that crossed our WELCOME: answer again, no log
                SendLobby(LOBBY_WELCOME, p.nonce, 0, from, slot);
                return;
            }
            // The same join attempt arriving on another path (a relay guest HELLOs the host's public AND LAN
            // endpoints): keep the path we locked and stay silent here, so the guest can only lock onto the
            // same path (it takes the first WELCOME's source). Found by the offline relay matrix (X1): a
            // re-lock here left host and guest on different paths and every game datagram was dropped.
            if (ready && p.nonce == g_lockedNonce) return;
            // Re-lock rule: the guest slot is taken by the first HELLO. A different source may take it
            // over only if it is the same machine (same IP, new NAT port after a guest restart) or the
            // current guest has been silent for 10 s. Through the relay every guest shares the server's
            // address, so "same machine" does not apply there: only silence frees the slot.
            const uint64_t now = GetTickCount64();
            const bool silent = g_lastRecvMs == 0 || now - g_lastRecvMs > 10000;
            const bool sameMachine = !viaRelay && !IsRelay(cur) && cur.sin_addr.s_addr == from.sin_addr.s_addr;
            if (ready && !sameMachine && !silent) {
                static int s_full = 0;
                if (s_full++ < 4) {
                    char ca[48]; FmtAddr(cur, ca, sizeof(ca));
                    _snprintf(m, sizeof(m), "[Lobby] HELLO from %s ignored: guest slot held by %s\n", fa, ca);
                    m[sizeof(m) - 1] = 0; dbg(m);
                }
                return;
            }
            InterlockedExchange(&g_peerReady, 0);
            PeerSet(from, slot);
            g_lockedNonce = p.nonce;
            g_lastRecvMs = 0;
            InterlockedExchange(&g_linkVia, viaRelay ? 2 : 1);
            InterlockedExchange(&g_peerReady, 1);
            SendLobby(LOBBY_WELCOME, p.nonce, 0, from, slot);
            _snprintf(m, sizeof(m), "[Lobby] guest joined %s (nonce=%08x codeCheck=%03x%s)\n", fa,
                      (unsigned)p.nonce, (unsigned)(p.codeCheck & 0xFFF), ready ? " re-lock" : "");
            m[sizeof(m) - 1] = 0; dbg(m);
            { char e[160]; _snprintf(e, sizeof(e), "[E] ev=lobby-join peer=%s\n", fa); e[sizeof(e) - 1] = 0; dbg(e); }
            return;
        }
        if (g_mode == LM_JOIN && (p.type == LOBBY_WELCOME || p.type == LOBBY_REJECT)) {
            const sockaddr_in cur = PeerCopy();
            // Our host is: the current peer; or, in a relay join, either endpoint PEER gave us, or the
            // relay itself (slot 0) - the WELCOME answers whichever path our HELLO took.
            const bool relayJoin = InterlockedCompareExchange64(&g_relayRoom, 0, 0) != 0;
            const bool hostEp = relayJoin && InterlockedCompareExchange(&g_hostInfo, 0, 0) &&
                                (SameAddr(from, g_hostPub) || SameAddr(from, g_hostLan));
            const bool ours = SameAddr(cur, from) || hostEp || (relayJoin && viaRelay && slot == Relay::kHostSlot);
            if (!ours || p.nonce != g_joinNonce) return;   // not our host / not this attempt
            if (InterlockedCompareExchange(&g_joinState, 0, 0) != 1) return;   // already decided
            if (p.type == LOBBY_WELCOME) {
                if (InterlockedCompareExchange(&g_joinState, 2, 1) != 1) return;
                PeerSet(from, Relay::kHostSlot);   // lock onto the path that answered
                InterlockedExchange(&g_linkVia, viaRelay ? 2 : 1);
                InterlockedExchange(&g_peerReady, 1);
                _snprintf(m, sizeof(m), "[Lobby] joined host %s\n", fa);
                m[sizeof(m) - 1] = 0; dbg(m);
                { char e[160]; _snprintf(e, sizeof(e), "[E] ev=lobby-joined host=%s\n", fa); e[sizeof(e) - 1] = 0; dbg(e); }
            } else {
                if (InterlockedCompareExchange(&g_joinState, 3, 1) != 1) return;
                _snprintf(m, sizeof(m), "[Lobby] join refused by %s: host proto=%u, ours=%u (different builds)\n",
                          fa, (unsigned)p.protoVer, (unsigned)NET_PROTO_VER);
                m[sizeof(m) - 1] = 0; dbg(m);
            }
        }
    }

    // Join mode: HELLO every 250 ms for the first 15 s (the design's window), then every 2 s for up to
    // 10 min, because in practice the host is often still loading when the guest reaches the world.
    // Deviation from lobby-code.md (15 s then stop) - the 15 s line is still logged as the design's verdict.
    unsigned __stdcall JoinThread(void* genArg) {
        const LONG gen = (LONG)(intptr_t)genArg;   // a newer F8 join attempt retires this thread
        const uint64_t t0 = GetTickCount64();
        int sent = 0;
        bool slowLogged = false;
        char pa[48]; FmtAddr(PeerCopy(), pa, sizeof(pa));
        char m[256];
        while (InterlockedCompareExchange(&g_joinState, 0, 0) == 1 && InterlockedCompareExchange(&g_joinGen, 0, 0) == gen) {
            const uint64_t el = GetTickCount64() - t0;
            if (el > 600000) {
                if (InterlockedCompareExchange(&g_joinGen, 0, 0) == gen && InterlockedCompareExchange(&g_joinState, 4, 1) == 1) {
                    _snprintf(m, sizeof(m), "[Lobby] join gave up: no answer from %s after %d HELLOs in 10 min\n", pa, sent);
                    m[sizeof(m) - 1] = 0; dbg(m);
                }
                break;
            }
            if (el > 15000 && !slowLogged) {
                slowLogged = true;
                _snprintf(m, sizeof(m), "[Lobby] no answer from %s after %d HELLOs in 15 s (host not in Mode=host yet, "
                          "or its router does not let the packet in) - still trying every 2 s for 10 min\n", pa, sent);
                m[sizeof(m) - 1] = 0; dbg(m);
            }
            SendLobby(LOBBY_HELLO, g_joinNonce, g_joinCheck, PeerCopy());
            ++sent;
            Sleep(el > 15000 ? 2000 : 250);
        }
        return 0;
    }

    // Start (or restart) a join attempt to ip:port. Called from Init (JoinCode=) and Net::JoinByCode (F8).
    // Ordering: state 0 first, so a late WELCOME for the previous attempt is ignored while nonce and peer
    // change; state 1 last publishes the new attempt.
    void StartJoin(uint32_t ipHost, uint16_t port, const char* codeText) {
        InterlockedExchange(&g_joinState, 0);
        InterlockedExchange(&g_peerReady, 0);
        InterlockedExchange64(&g_relayRoom, 0);   // a direct code: not a relay join
        InterlockedExchange(&g_hostInfo, 0);
        InterlockedExchange(&g_linkVia, 0);
        sockaddr_in peer{}; peer.sin_family = AF_INET; peer.sin_port = htons(port); peer.sin_addr.s_addr = htonl(ipHost);
        PeerSet(peer);
        LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
        g_joinNonce = (uint32_t)qpc.LowPart ^ (GetCurrentProcessId() << 16) ^ (uint32_t)GetTickCount64();
        g_joinCheck = CodeCheckOf(ipHost, port);
        AcquireSRWLockExclusive(&g_codeLock);
        lstrcpynA(g_joinCodeText, codeText, sizeof(g_joinCodeText));
        ReleaseSRWLockExclusive(&g_codeLock);
        const LONG gen = InterlockedIncrement(&g_joinGen);
        InterlockedExchange(&g_joinState, 1);
        char pa[48]; FmtAddr(peer, pa, sizeof(pa));
        char m[160];
        _snprintf(m, sizeof(m), "[Lobby] joining %s -> host %s (nonce=%08x)\n", codeText, pa, (unsigned)g_joinNonce);
        m[sizeof(m) - 1] = 0; dbg(m);
        _beginthreadex(nullptr, 0, JoinThread, (void*)(intptr_t)gen, 0, nullptr);
    }

    void SetHostCode(const char* code) {
        AcquireSRWLockExclusive(&g_codeLock);
        lstrcpynA(g_hostCode, code, sizeof(g_hostCode));
        ReleaseSRWLockExclusive(&g_codeLock);
    }
    void FmtIp(uint32_t ip, char* out, size_t n) {
        _snprintf(out, n, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
        out[n - 1] = 0;
    }

    // ---- relay (docs/design/relay.md) ------------------------------------------------------------------
    // Resolve [Net] RelayServer once, off the game thread (DNS can block). Default port 47800.
    bool RelayResolve() {
        const LONG st = InterlockedCompareExchange(&g_relayResolved, 0, 0);
        if (st) return st == 1;
        char hp[160]; lstrcpynA(hp, g_relayName, sizeof(hp));
        if (!strchr(hp, ':')) { size_t l = strlen(hp); _snprintf(hp + l, sizeof(hp) - l, ":47800"); hp[sizeof(hp) - 1] = 0; }
        sockaddr_in a{};
        char m[256];
        if (!Stun::Resolve(hp, &a)) {
            InterlockedExchange(&g_relayResolved, -1);
            _snprintf(m, sizeof(m), "[Lobby] relay server \"%s\" did not resolve - room codes are off\n", hp);
            m[sizeof(m) - 1] = 0; dbg(m);
            return false;
        }
        g_relaySrv = a;
        InterlockedExchange(&g_relayResolved, 1);
        char sa[48]; FmtAddr(a, sa, sizeof(sa));
        _snprintf(m, sizeof(m), "[Lobby] relay server %s = %s\n", hp, sa); m[sizeof(m) - 1] = 0; dbg(m);
        return true;
    }
    uint32_t NewNonce() {
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        return (uint32_t)q.LowPart ^ (GetCurrentProcessId() << 16) ^ (uint32_t)GetTickCount64() ^ 0x9E3779B9u;
    }
    void RelayHello(uint8_t type, uint64_t room) {
        uint32_t lan = 0; Upnp::DefaultRouteIPv4(&lan);
        Relay::Hello h{ NET_PROTO_VER, lan, (uint16_t)g_bindPort, 0, g_relayNonce, 0, 0 };
        RelaySend(type, 0, room, &h, (int)sizeof(h));
    }
    void RoomCode(uint64_t room, char out[15]) {
        uint32_t ip = 0; uint16_t port = 0;
        Relay::AddressFromRoom(room, &ip, &port);
        LobbyCode::Encode(ip, port, out);
    }
    sockaddr_in Sa(uint32_t ipHost, uint16_t port) {
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(ipHost); a.sin_port = htons(port);
        return a;
    }
    const char* RelayErrText(LONG code) {
        switch (code) {
            case Relay::E_UNKNOWN_ROOM: return "no such room (expired, mistyped, or a different relay server)";
            case Relay::E_ROOM_FULL:    return "room full";
            case Relay::E_PROTO:        return "the host runs a different build";
            case Relay::E_VERSION:      return "the relay server is a different version";
            case Relay::E_NOT_MEMBER:   return "not a member of that room";
            case 100:                   return "relay server name did not resolve";
            default:                    return "relay error";
        }
    }

    // Host: punch towards a guest the relay introduced (opens our NAT for its endpoints). 3 s, both endpoints.
    struct PunchArgs { sockaddr_in pub, lan; uint32_t nonce; };
    unsigned __stdcall PunchThread(void* arg) {
        PunchArgs* a = (PunchArgs*)arg;
        for (int i = 0; i < 12 && InterlockedCompareExchange(&g_peerReady, 0, 0) == 0; ++i) {
            SendLobby(LOBBY_PUNCH, a->nonce, 0, a->pub);
            if (a->lan.sin_addr.s_addr && a->lan.sin_port && !SameAddr(a->lan, a->pub)) SendLobby(LOBBY_PUNCH, a->nonce, 0, a->lan);
            Sleep(250);
        }
        delete a;
        return 0;
    }

    // Guest: keep the room membership (and our NAT mapping towards the server) alive, every 15 s.
    unsigned __stdcall RelayKeepThread(void*) {
        for (;;) {
            const uint64_t room = (uint64_t)InterlockedCompareExchange64(&g_relayRoom, 0, 0);
            if (room && InterlockedCompareExchange(&g_relayResolved, 0, 0) == 1) RelaySend(Relay::T_KEEPALIVE, 0, room, nullptr, 0);
            Sleep(15000);
        }
    }

    // A relay control datagram from the relay server (recv thread). DATA never comes here.
    void HandleRelayControl(const Relay::Hdr& h, const uint8_t* pl, int pn) {
        char m[256], a[48], b[48];
        if (h.type == Relay::T_REGISTERED && g_mode == LM_HOST && pn >= (int)sizeof(Relay::Registered)) {
            Relay::Registered r; std::memcpy(&r, pl, sizeof(r));
            if (r.nonce != g_relayNonce) return;
            const uint64_t room = h.room & 0xFFFFFFFFFFull;
            const uint64_t prev = (uint64_t)InterlockedExchange64(&g_relayRoom, (LONG64)room);
            if (!InterlockedExchange(&g_relayRegistered, 1) || prev != room) {
                char code[15] = {0}; RoomCode(room, code);
                SetHostCode(code);
                FmtAddr(g_relaySrv, a, sizeof(a));
                _snprintf(m, sizeof(m), "[Lobby] relay room %010llx registered at %s: room code %s\n", (unsigned long long)room, a, code);
                m[sizeof(m) - 1] = 0; dbg(m);
                { char e[96]; _snprintf(e, sizeof(e), "[E] ev=relay-room code=%s\n", code); e[sizeof(e) - 1] = 0; dbg(e); }
            }
        } else if (h.type == Relay::T_PEER && pn >= (int)sizeof(Relay::Peer)) {
            Relay::Peer p; std::memcpy(&p, pl, sizeof(p));
            const sockaddr_in pub = Sa(p.pubIp, p.pubPort), lan = Sa(p.lanIp, p.lanPort);
            FmtAddr(pub, a, sizeof(a)); FmtAddr(lan, b, sizeof(b));
            if (g_mode == LM_HOST) {
                _snprintf(m, sizeof(m), "[Lobby] relay: guest slot %u is %s (LAN %s)%s\n", (unsigned)h.slot, a, b,
                          g_relayForce ? " - RelayForce=1, no punch" : " - punching");
                m[sizeof(m) - 1] = 0; dbg(m);
                if (!g_relayForce) _beginthreadex(nullptr, 0, PunchThread, new PunchArgs{ pub, lan, p.nonce }, 0, nullptr);
            } else if (g_mode == LM_JOIN && p.nonce == g_relayNonce && !InterlockedCompareExchange(&g_hostInfo, 0, 0)) {
                g_hostPub = pub; g_hostLan = lan;
                InterlockedExchange(&g_hostInfo, 1);
                _snprintf(m, sizeof(m), "[Lobby] relay: host is %s (LAN %s)\n", a, b); m[sizeof(m) - 1] = 0; dbg(m);
            }
        } else if (h.type == Relay::T_ERR && pn >= (int)sizeof(Relay::Error)) {
            Relay::Error e; std::memcpy(&e, pl, sizeof(e));
            if (e.nonce != g_relayNonce) return;
            InterlockedExchange(&g_relayErr, e.code);
            _snprintf(m, sizeof(m), "[Lobby] relay server refused: %s\n", RelayErrText(e.code)); m[sizeof(m) - 1] = 0; dbg(m);
            if (g_mode == LM_JOIN) InterlockedCompareExchange(&g_joinState, 5, 1);
            else if (g_mode == LM_HOST && e.code == Relay::E_UNKNOWN_ROOM) {
                // Our room id was taken over (we were silent too long): register a new room on the next beat.
                InterlockedExchange64(&g_relayRoom, 0);
                InterlockedExchange(&g_relayRegistered, 0);
            }
        }
    }

    // Guest, room code: ask the relay for the host (JOIN until PEER), try the host directly for 3 s
    // (unless RelayForce), then fall back to HELLO through the relay with the usual retry schedule.
    unsigned __stdcall RelayJoinThread(void* genArg) {
        const LONG gen = (LONG)(intptr_t)genArg;
        auto alive = [&] { return InterlockedCompareExchange(&g_joinState, 0, 0) == 1 && InterlockedCompareExchange(&g_joinGen, 0, 0) == gen; };
        char m[256];
        if (!RelayResolve()) { InterlockedExchange(&g_relayErr, 100); InterlockedCompareExchange(&g_joinState, 5, 1); return 0; }
        static volatile LONG s_keep = 0;
        if (!InterlockedExchange(&s_keep, 1)) _beginthreadex(nullptr, 0, RelayKeepThread, nullptr, 0, nullptr);
        const uint64_t room = (uint64_t)InterlockedCompareExchange64(&g_relayRoom, 0, 0);
        PeerSet(g_relaySrv, Relay::kHostSlot);
        uint64_t t0 = GetTickCount64();
        while (alive() && !InterlockedCompareExchange(&g_hostInfo, 0, 0) && GetTickCount64() - t0 < 15000) {
            RelayHello(Relay::T_JOIN, room);
            Sleep(500);
        }
        if (!alive()) return 0;
        if (!InterlockedCompareExchange(&g_hostInfo, 0, 0)) {
            if (InterlockedCompareExchange(&g_joinState, 4, 1) == 1) dbg("[Lobby] join: the relay server did not answer in 15 s\n");
            return 0;
        }
        if (!g_relayForce) {
            PeerSet(g_hostPub, Relay::kHostSlot);
            t0 = GetTickCount64();
            while (alive() && GetTickCount64() - t0 < 3000) {
                SendLobby(LOBBY_HELLO, g_joinNonce, g_joinCheck, g_hostPub);
                if (g_hostLan.sin_addr.s_addr && g_hostLan.sin_port && !SameAddr(g_hostLan, g_hostPub))
                    SendLobby(LOBBY_HELLO, g_joinNonce, g_joinCheck, g_hostLan);
                Sleep(250);
            }
            if (!alive()) return 0;   // WELCOMEd directly (or superseded)
            dbg("[Lobby] join: no direct answer in 3 s - using the relay\n");
        } else {
            dbg("[Lobby] join: RelayForce=1 - using the relay\n");
        }
        PeerSet(g_relaySrv, Relay::kHostSlot);
        t0 = GetTickCount64();
        int sent = 0; bool slowLogged = false;
        while (alive()) {
            const uint64_t el = GetTickCount64() - t0;
            if (el > 600000) {
                if (InterlockedCompareExchange(&g_joinState, 4, 1) == 1) dbg("[Lobby] join gave up: no WELCOME through the relay in 10 min\n");
                break;
            }
            if (el > 15000 && !slowLogged) {
                slowLogged = true;
                _snprintf(m, sizeof(m), "[Lobby] no WELCOME through the relay after %d HELLOs in 15 s - still trying every 2 s\n", sent);
                m[sizeof(m) - 1] = 0; dbg(m);
            }
            SendLobby(LOBBY_HELLO, g_joinNonce, g_joinCheck, g_relaySrv, Relay::kHostSlot);
            ++sent;
            Sleep(el > 15000 ? 2000 : 250);
        }
        return 0;
    }

    void StartRelayJoin(uint64_t room, const char* codeText) {
        InterlockedExchange(&g_joinState, 0);
        InterlockedExchange(&g_peerReady, 0);
        InterlockedExchange(&g_hostInfo, 0);
        InterlockedExchange(&g_relayErr, 0);
        InterlockedExchange(&g_linkVia, 0);
        InterlockedExchange64(&g_relayRoom, (LONG64)room);
        g_relayNonce = NewNonce();
        g_joinNonce = NewNonce() ^ 0x5bd1e995u;
        g_joinCheck = 0;
        AcquireSRWLockExclusive(&g_codeLock);
        lstrcpynA(g_joinCodeText, codeText, sizeof(g_joinCodeText));
        ReleaseSRWLockExclusive(&g_codeLock);
        const LONG gen = InterlockedIncrement(&g_joinGen);
        InterlockedExchange(&g_joinState, 1);
        char m[160];
        _snprintf(m, sizeof(m), "[Lobby] joining room %s (%010llx) through relay %s\n", codeText, (unsigned long long)room, g_relayName);
        m[sizeof(m) - 1] = 0; dbg(m);
        _beginthreadex(nullptr, 0, RelayJoinThread, (void*)(intptr_t)gen, 0, nullptr);
    }

    // One STUN Binding request out of the game socket; the recv thread completes it. Returns true when
    // an answer arrived within waitMs.
    bool StunOnce(const sockaddr_in& srv, int waitMs) {
        InterlockedExchange(&g_stunPending, 0);
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        uint32_t seed = q.LowPart ^ (GetCurrentProcessId() << 8) ^ (uint32_t)GetTickCount64();
        for (int i = 0; i < 12; ++i) { seed = seed * 1103515245u + 12345u; g_stunTid[i] = (uint8_t)(seed >> 16); }
        InterlockedExchange(&g_stunDone, 0);
        InterlockedExchange(&g_stunPending, 1);
        uint8_t req[20]; Stun::BuildRequest(req, g_stunTid);
        sendto(g_sock, (const char*)req, sizeof(req), 0, (const sockaddr*)&srv, sizeof(srv));
        const ULONGLONG t0 = GetTickCount64();
        while (GetTickCount64() - t0 < (ULONGLONG)waitMs) {
            if (InterlockedCompareExchange(&g_stunDone, 0, 0)) return true;
            Sleep(20);
        }
        InterlockedExchange(&g_stunPending, 0);
        return false;
    }

    // Host mode background work: UPnP mapping, STUN check, the verdict and the code; then keep the NAT
    // mappings alive (STUN every 25 s until a guest joins; UPnP lease renewed every 30 min). Never on the
    // game thread. Removal on quit is NOT done: the only exit hook is DLL_PROCESS_DETACH, where WinHTTP
    // must not run (loader lock). The 7200 s lease expires on its own, and the next host start deletes a
    // stale "AC4Coop" mapping of this PC before adding a new one.
    unsigned __stdcall HostThread(void*) {
        char m[384], err[256], a[32], b[32];
        const uint16_t port = (uint16_t)g_bindPort;
        uint32_t lan = 0;
        const bool haveLan = Upnp::DefaultRouteIPv4(&lan);

        // ---- relay room first (fast: the room code shows within a second) ----
        const bool relay = g_relayName[0] && RelayResolve();
        if (relay) {
            g_relayNonce = NewNonce();
            for (int i = 0; i < 10 && !InterlockedCompareExchange(&g_relayRegistered, 0, 0); ++i) {
                RelayHello(Relay::T_REGISTER, 0);
                Sleep(1000);
            }
            if (!InterlockedCompareExchange(&g_relayRegistered, 0, 0))
                dbg("[Lobby] relay server did not answer REGISTER in 10 s - still retrying every 15 s; direct codes below\n");
        }

        // ---- UPnP ----
        Upnp::Gateway gw{};
        bool upnpGw = false, mapped = false, haveExt = false;
        uint16_t extPort = 0; uint32_t ext = 0, lease = 0;
        if (!g_upnpEnabled) dbg("[Lobby] UPnP off ([Net] Upnp=0)\n");
        else if (!haveLan) dbg("[Lobby] UPnP skipped: no LAN address\n");
        else if (!(upnpGw = Upnp::Discover(lan, &gw, err, sizeof(err)))) {
            _snprintf(m, sizeof(m), "[Lobby] UPnP: %s\n", err); m[sizeof(m) - 1] = 0; dbg(m);
        } else {
            _snprintf(m, sizeof(m), "[Lobby] UPnP gateway %s (%s)\n", gw.location, gw.service); m[sizeof(m) - 1] = 0; dbg(m);
            for (int k = 0; k < 10; ++k) {   // stale mappings this PC left behind (crash / quit without cleanup)
                uint32_t cl = 0; uint16_t ip2 = 0; char desc[64] = {0};
                const uint16_t p = (uint16_t)(port + k);
                if (Upnp::GetUdpMapping(gw, p, &cl, &ip2, desc, sizeof(desc), err, sizeof(err)) && cl == lan && strcmp(desc, "AC4Coop") == 0) {
                    const bool del = Upnp::DeleteUdpMapping(gw, p, err, sizeof(err));
                    _snprintf(m, sizeof(m), "[Lobby] UPnP removed stale AC4Coop mapping UDP %u: %s\n", p, del ? "ok" : err);
                    m[sizeof(m) - 1] = 0; dbg(m);
                }
            }
            int ue = 0;
            mapped = Upnp::AddUdpMappingAny(gw, port, 7200, "AC4Coop", &extPort, &ue, &lease, err, sizeof(err));
            if (mapped) _snprintf(m, sizeof(m), "[Lobby] UPnP mapped UDP %u -> this PC:%u (lease %u s%s)\n", extPort, port,
                                  lease, extPort != port ? ", port conflict skipped" : "");
            else _snprintf(m, sizeof(m), "[Lobby] UPnP mapping refused: %s\n", err);
            m[sizeof(m) - 1] = 0; dbg(m);
            haveExt = Upnp::GetExternalIp(gw, &ext, err, sizeof(err));
            if (haveExt) { FmtIp(ext, a, sizeof(a)); _snprintf(m, sizeof(m), "[Lobby] UPnP external address %s\n", a); }
            else _snprintf(m, sizeof(m), "[Lobby] UPnP external address unknown: %s\n", err);
            m[sizeof(m) - 1] = 0; dbg(m);
        }

        // ---- STUN ----
        sockaddr_in srv{};
        bool haveSrv = false, stun = false;
        if (!g_stunServer[0]) dbg("[Lobby] STUN off ([Net] StunServer= empty)\n");
        else if (!(haveSrv = Stun::Resolve(g_stunServer, &srv))) {
            _snprintf(m, sizeof(m), "[Lobby] STUN server %s did not resolve\n", g_stunServer); m[sizeof(m) - 1] = 0; dbg(m);
        } else {
            for (int i = 0; i < 3 && !stun; ++i) stun = StunOnce(srv, 1000);
            if (stun) { FmtIp(g_stunIp, a, sizeof(a)); _snprintf(m, sizeof(m), "[Lobby] STUN: the internet sees %s:%u\n", a, g_stunPort); }
            else _snprintf(m, sizeof(m), "[Lobby] STUN: no answer from %s in 3 tries\n", g_stunServer);
            m[sizeof(m) - 1] = 0; dbg(m);
        }

        // ---- verdict and code ----
        const char* verdict; const char* advice; uint32_t cIp = 0; uint16_t cPort = 0;
        if (!stun) {
            verdict = "NO_STUN"; advice = "internet address unknown; only the LAN code below is known to work";
        } else if (haveLan && lan == g_stunIp) {
            verdict = "PUBLIC"; advice = "this PC is directly on the internet; friends can join with the code";
            cIp = g_stunIp; cPort = port;
        } else if (mapped && haveExt && ext == g_stunIp) {
            verdict = "OK"; advice = "single router, UPnP opened the port; friends can join with the code";
            cIp = g_stunIp; cPort = extPort;
        } else if (haveExt && Upnp::AddressClass(ext) == 2) {
            verdict = "CGNAT"; advice = "carrier-grade NAT at the ISP: friends outside cannot reach this PC; let a friend host";
            cIp = g_stunIp; cPort = g_stunPort;
        } else if (haveExt && Upnp::AddressClass(ext) == 1) {
            verdict = "DOUBLE_NAT"; advice = "this router sits behind another one: friends outside probably cannot join; "
                                             "let a friend with a simple router host, or forward UDP on the outer router";
            cIp = g_stunIp; cPort = g_stunPort;
        } else {
            verdict = "NO_UPNP"; advice = "the router did not open the port: forward UDP to this PC by hand, or let a friend host";
            cIp = g_stunIp; cPort = g_stunPort;
        }
        const bool roomCode = InterlockedCompareExchange(&g_relayRegistered, 0, 0) != 0;
        if (roomCode) advice = "the relay room code is the one to share; this direct code is an alternative";
        if (cIp) {
            char code[15] = {0}; LobbyCode::Encode(cIp, cPort, code);
            if (!roomCode) SetHostCode(code);
            FmtIp(cIp, a, sizeof(a));
            _snprintf(m, sizeof(m), "[Lobby] host internet code %s (%s:%u) verdict=%s - %s\n", code, a, cPort, verdict, advice);
        } else {
            _snprintf(m, sizeof(m), "[Lobby] host verdict=%s - %s\n", verdict, advice);
        }
        m[sizeof(m) - 1] = 0; dbg(m);
        { char e[96]; _snprintf(e, sizeof(e), "[E] ev=lobby-host verdict=%s\n", verdict); e[sizeof(e) - 1] = 0; dbg(e); }
        // For the status line / panel: when no code exists at all, say why instead of "getting code..." forever.
        InterlockedExchangePointer((PVOID volatile*)&g_hostNote, (PVOID)(cIp ? "" : advice));
        InterlockedExchange(&g_hostDone, 1);

        // ---- keep the mappings alive ----
        ULONGLONG lastStun = GetTickCount64(), lastMap = GetTickCount64(), lastRelay = GetTickCount64();
        uint32_t prevIp = g_stunIp; uint16_t prevPort = g_stunPort;
        for (;;) {
            Sleep(1000);
            const ULONGLONG now = GetTickCount64();
            // Relay: re-REGISTER every 15 s (keeps the room and our NAT mapping towards the server alive);
            // every 2 s while not registered (first answer lost, or the room was taken over).
            if (relay && now - lastRelay >= (InterlockedCompareExchange(&g_relayRegistered, 0, 0) ? 15000u : 2000u)) {
                lastRelay = now;
                RelayHello(Relay::T_REGISTER, (uint64_t)InterlockedCompareExchange64(&g_relayRoom, 0, 0));
            }
            if (haveSrv && InterlockedCompareExchange(&g_peerReady, 0, 0) == 0 && now - lastStun >= 25000) {
                lastStun = now;
                if (StunOnce(srv, 1000) && (g_stunIp != prevIp || g_stunPort != prevPort)) {
                    FmtIp(prevIp, a, sizeof(a)); FmtIp(g_stunIp, b, sizeof(b));
                    _snprintf(m, sizeof(m), "[Lobby] STUN: public mapping changed %s:%u -> %s:%u (the old code no longer works)\n",
                              a, prevPort, b, g_stunPort);
                    m[sizeof(m) - 1] = 0; dbg(m);
                    prevIp = g_stunIp; prevPort = g_stunPort;
                }
            }
            if (mapped && lease != 0 && now - lastMap >= 1800000) {
                lastMap = now;
                int ue = 0; uint32_t l2 = 0;
                if (!Upnp::AddUdpMapping(gw, extPort, port, 7200, "AC4Coop", &ue, &l2, err, sizeof(err))) {
                    _snprintf(m, sizeof(m), "[Lobby] UPnP lease renewal failed: %s\n", err); m[sizeof(m) - 1] = 0; dbg(m);
                }
            }
        }
    }

    // Lobby handshake selftest: the frozen 20-byte layout and the constants the Python twin pins.
    void SelfTestLobby() {
        LobbyPacket p{ NET_LOBBY_MAGIC, NET_PROTO_VER, LOBBY_HELLO, 0x01020304u, 0x0ABCu };
        uint8_t raw[sizeof(p)];
        std::memcpy(raw, &p, sizeof(p));
        // Little-endian on the wire: 'A','C','4','L' then the version.
        const bool ok = sizeof(LobbyPacket) == 20 && raw[0] == 'A' && raw[1] == 'C' && raw[2] == '4' && raw[3] == 'L' &&
                        raw[4] == (uint8_t)NET_PROTO_VER && raw[8] == 1 && raw[12] == 0x04 && raw[15] == 0x01 &&
                        // 0x5E7 = the low 12 bits of SC07-21XT-Q5F7 ('5'&3, 'F', '7'), the vector for 203.0.113.7:47801
                        CodeCheckOf(0xCB007107u, 47801) == 0x5E7u;
        char m[128];
        _snprintf(m, sizeof(m), "[Lobby] handshake selftest: %s proto=%u\n", ok ? "PASS" : "FAIL", (unsigned)NET_PROTO_VER);
        m[sizeof(m) - 1] = 0; dbg(m);
    }

    DWORD WINAPI RecvThread(LPVOID) {
        // Read into a buffer sized for the LARGEST packet type (EntityPacket), then dispatch on the
        // leading magic. Both PlayerState and EntityPacket share the one UDP socket.
        char buf[16384];
        for (;;) {
            sockaddr_in from{}; int flen = sizeof(from);
            int n = recvfrom(g_sock, buf, sizeof(buf), 0, (sockaddr*)&from, &flen);
            if (n >= 4) {
                uint32_t magic = *(uint32_t*)buf;
                // STUN answer for the host thread (slice 5). A game datagram can never pass LooksLikeStun:
                // byte 0 of 'AC4C'/'AC4E'/'AC4L' is 'A' (0x41), whose top bits are not 00. Host mode only.
                if (g_mode == LM_HOST && Stun::LooksLikeStun((const uint8_t*)buf, n)) {
                    if (InterlockedCompareExchange(&g_stunPending, 0, 0)) {
                        uint32_t sip = 0; uint16_t sport = 0;
                        if (Stun::ParseResponse((const uint8_t*)buf, n, g_stunTid, &sip, &sport)) {
                            g_stunIp = sip; g_stunPort = sport;
                            InterlockedExchange(&g_stunPending, 0);
                            InterlockedExchange(&g_stunDone, 1);
                        }
                    }
                    continue;
                }
                // Relay lane (docs/design/relay.md): only from the configured relay server, lobby modes only.
                // Control messages go to HandleRelayControl; DATA is unwrapped here, before every other check,
                // so what follows sees the inner datagram exactly as on a direct link, with `from` = the server
                // and rxSlot = the member it came from.
                uint16_t rxSlot = Relay::kHostSlot;
                if (magic == Relay::kMagic && g_mode != LM_MANUAL && n >= (int)sizeof(Relay::Hdr) && IsRelay(from)) {
                    Relay::Hdr rh; std::memcpy(&rh, buf, sizeof(rh));
                    if (rh.version != Relay::kVersion) continue;
                    if (rh.type != Relay::T_DATA) {
                        HandleRelayControl(rh, (const uint8_t*)buf + sizeof(rh), n - (int)sizeof(rh));
                        continue;
                    }
                    if ((rh.room & 0xFFFFFFFFFFull) != (uint64_t)InterlockedCompareExchange64(&g_relayRoom, 0, 0)) continue;
                    rxSlot = rh.slot;
                    n -= (int)sizeof(rh);
                    if (n < 4) continue;
                    std::memmove(buf, buf + sizeof(rh), (size_t)n);
                    magic = *(uint32_t*)buf;
                }
                // Lobby lane (slice 3): handled first and never counted as game traffic. Manual mode
                // ignores lobby packets entirely.
                if (magic == NET_LOBBY_MAGIC) {
                    if (g_mode != LM_MANUAL && n == (int)sizeof(LobbyPacket)) {
                        LobbyPacket lp; std::memcpy(&lp, buf, sizeof(lp));
                        HandleLobby(lp, from, rxSlot);
                    }
                    continue;
                }
                // Before the handshake there is no peer: drop game packets unseen (lobby modes only).
                if (InterlockedCompareExchange(&g_peerReady, 0, 0) == 0) continue;
                uint16_t peerSlot = 0;
                const sockaddr_in peer = PeerCopy(&peerSlot);   // one consistent copy per datagram (g_peerLock)
                // Relayed peer: other members of the room arrive from the same server endpoint; only the
                // locked member's slot is our peer.
                if (IsRelay(peer) && IsRelay(from) && rxSlot != peerSlot) continue;
                // X4b: player SIZE mismatch, named instead of anonymous. A peer on a different build
                // (older/newer struct) sends our player magic with a foreign length, so playerPkt is
                // false and `if (playerPkt && !playerOk)` below never runs: no protoMismatch line, no
                // reject line, no evidence at all. Detect it from the magic alone (before the length
                // gate decides anything), count it, and log once per process. Same source filtering
                // as the strict-peer gate so foreign traffic cannot trip the one-shot log.
                if (magic == NET_MAGIC && n != (int)sizeof(PlayerState) &&
                    (!g_strictPeer || from.sin_addr.s_addr == peer.sin_addr.s_addr)) {
                    uint32_t peerProto = 0;
                    const bool haveProto = (n >= 8);            // protoVer is the second u32
                    if (haveProto) std::memcpy(&peerProto, buf + 4, sizeof(peerProto));
                    InterlockedIncrement(&g_rxSizeMismatch);
                    LogSizeMismatch(n, haveProto, peerProto);
                }
                bool playerPkt = (magic == NET_MAGIC        && n == (int)sizeof(PlayerState));
                bool entityPkt = (magic == NET_ENTITY_MAGIC && n >= ENTITY_WIRE_HEADER_SIZE);
                if (playerPkt || entityPkt) {
                    // Strict peer (Feature #3): accept only datagrams from the configured PeerIP.
                    if (g_strictPeer && from.sin_addr.s_addr != peer.sin_addr.s_addr) {
                        static bool warned = false;
                        if (!warned) {
                            warned = true;
                            char got[32] = {0}, exp[32] = {0};
                            inet_ntop(AF_INET, &from.sin_addr, got, sizeof(got));
                            inet_ntop(AF_INET, &peer.sin_addr, exp, sizeof(exp));
                            char m[160];
                            wsprintfA(m, "[Net] dropped packet from unexpected source %s (expected %s)\n", got, exp);
                            dbg(m);
                        }
                    } else {
                        EntityPacket decoded{};
                        int killDrops = 0;   // kill-ring entries overwritten by this datagram
                        // Both packet types put magic first and protoVer second, so the version is
                        // readable before the type-specific decode (a player datagram is exactly
                        // sizeof(PlayerState) = 576 B, an entity one >= ENTITY_WIRE_HEADER_SIZE, so
                        // bytes 4..8 are always inside it).
                        uint32_t peerProto = NET_PROTO_VER;
                        std::memcpy(&peerProto, buf + 4, sizeof(peerProto));
                        // Player lane (X4): magic+length alone let a PROTO-9 peer through. 9 -> 10
                        // turned shipHP from int32 to float WITHOUT changing sizeof(PlayerState), so
                        // the length check cannot see it and an int hull reads back as a denormal.
                        const bool playerOk = playerPkt && peerProto == NET_PROTO_VER;
                        // Entity lane (X18/X15/X5): the chunk decoder reports WHY it dropped or deferred.
                        DecodeReason why = DEC_OK;
                        const bool entityOk = entityPkt &&
                            (DecodeEntityChunk((const uint8_t*)buf, (size_t)n, decoded, &why) == DEC_OK);
                        const bool entityPending = entityPkt && !entityOk && why == DEC_PENDING;
                        // DEC_STALE: a late/reordered chunk of a superseded snapshot. Correct to drop,
                        // but benign on a well-formed link -> its own counter, never a reject.
                        const bool entityStale   = entityPkt && !entityOk && why == DEC_STALE;
                        const bool accepted = playerPkt ? playerOk : entityOk;
                        if (playerPkt && !playerOk) {
                            InterlockedIncrement(&g_rxProtoMismatch);
                            LogProtoMismatch("player", peerProto);            // one line per process
                        } else if (entityPkt && !entityOk) {
                            if (why == DEC_PROTO) {
                                InterlockedIncrement(&g_rxProtoMismatch);
                                LogProtoMismatch("entity", peerProto);        // one line per process
                            } else if (why == DEC_KIND) {
                                LogKindRejected();
                            } else if (why == DEC_WORLD) {
                                LogWorldMismatch();
                            }
                            // DEC_PENDING: a chunk buffered for reassembly - neither accepted nor rejected.
                            // DEC_STALE: a superseded chunk - dropped, and counted as stale, not as a reject.
                        }
                        if (entityStale) InterlockedIncrement(&g_rxStaleChunks);
                        if (!accepted && !entityPending && !entityStale) {
                            const LONG rejects = InterlockedIncrement(&g_rxRejects);
                            if (rejects == 1 || (rejects % 256) == 0) LogRejectSummary(rejects);
                        }
                        EnterCriticalSection(&g_lock);
                        if (playerOk) {
                            std::memcpy(&g_remote, buf, sizeof(g_remote));   // not *(PlayerState*)buf: buf is not 16-byte aligned
                            // blendCount indexes blendFactors[BLEND_MAX] in lerpState/apply; a wire value above it is an OOB write.
                            if (g_remote.blendCount > BLEND_MAX) g_remote.blendCount = BLEND_MAX;
                            g_haveRemote = true;
                            // Queue it too (overwrites the OLDEST when the ring is full), so a frame
                            // that drains several packets feeds the interpolator all of them.
                            g_rxq[g_rxqHead] = g_remote;
                            g_rxqHead = (g_rxqHead + 1) % NET_RXQ;
                            if (g_rxqCount < NET_RXQ) ++g_rxqCount;
                        }
                        else if (entityPkt && entityOk) {
                            // B-4: pull the death entries out into their own ring first. A packet made
                            // only of them (the dedicated one-entry death push) does not touch the
                            // snapshot slot at all, so it can neither be overwritten before the frame
                            // reads it nor clobber a snapshot.
                            bool hadNonKill = false;
                            for (uint32_t ki = 0; ki < decoded.count; ++ki) {
                                const EntityState& ks = decoded.ents[ki];
                                if (ks.kind != NET_KIND_KILL) { hadNonKill = true; continue; }
                                // A full ring silently overwrote the OLDEST death - that is how a death
                                // could vanish with no trace. Count it here, log the first few below
                                // (outside the lock).
                                if (g_killCount == NET_KILLQ) ++killDrops;
                                g_killRing[g_killHead] = ks;
                                g_killHead = (g_killHead + 1) % NET_KILLQ;
                                if (g_killCount < NET_KILLQ) ++g_killCount;
                            }
                            if (hadNonKill || decoded.count == 0) {
                                g_remoteEntities = decoded; g_haveEntities = true;
                                g_entitiesMs = GetTickCount64();
                            }
                        }
                        // A header-only entity packet carries no state at all: it is a heartbeat, not
                        // evidence that the peer is alive, so it must NOT refresh the presence timer.
                        // A REJECTED datagram refreshes nothing either (X18): it used to bump
                        // g_lastRecvMs, so a peer whose every snapshot failed to decode looked alive.
                        // rxPackets/rxBytes still count it - it did arrive on the wire - and
                        // g_rxRejects is the subset that produced no state, EXCLUDING the chunks that
                        // were merely deferred (DEC_PENDING) or superseded (DEC_STALE).
                        const bool heartbeatOnly = accepted && entityPkt && decoded.count == 0;
                        if (accepted && !heartbeatOnly) g_lastRecvMs = GetTickCount64();
                        InterlockedIncrement(&g_rxPackets);
                        InterlockedAdd(&g_rxBytes, (LONG)n);
                        LeaveCriticalSection(&g_lock);
                        if (killDrops) {
                            static int s_dropLogs = 0;
                            if (s_dropLogs < NET_KILL_DROP_LOGS) {
                                ++s_dropLogs;
                                char m[128];
                                wsprintfA(m, "[Net] kill ring FULL: oldest death overwritten (drop #%d)\n",
                                          s_dropLogs);
                                dbg(m);
                            }
                        }
                    }
                }
            } else if (n == SOCKET_ERROR) {
                int e = WSAGetLastError();
                if (e == WSAEINTR || e == WSAENOTSOCK) break;   // socket closed
                Sleep(1);
            }
        }
        return 0;
    }
}

namespace {
    // Host mode setup: log the LAN and same-PC codes, then the host thread (relay room, UPnP, STUN).
    // From Init (Mode=host) or from Net::StartHosting (Mode=lobby, the panel's Host button).
    void BeginHosting(int bindPort) {
        char m[192];
        { char sm[160]; Stun::SelfTest(sm, sizeof(sm)); dbg(sm); }
        uint32_t lan = 0;
        bool haveLan = Upnp::DefaultRouteIPv4(&lan);
        if (!haveLan) {
            // Second way: this computer's own IPv4 addresses (first that is not loopback / link-local).
            // A friend reported "getting code..." forever: the only way to get there is no route address AND
            // no STUN answer; their log is still needed to know which failed (UNVERIFIED cause).
            char hn[256] = {0};
            addrinfo hints{}; hints.ai_family = AF_INET; addrinfo* res = nullptr;
            if (gethostname(hn, sizeof(hn)) == 0 && getaddrinfo(hn, nullptr, &hints, &res) == 0) {
                for (addrinfo* a = res; a && !haveLan; a = a->ai_next) {
                    const uint32_t ip = ntohl(((sockaddr_in*)a->ai_addr)->sin_addr.s_addr);
                    if ((ip >> 24) != 127 && (ip >> 16) != 0xA9FE && ip != 0) { lan = ip; haveLan = true; }
                }
                freeaddrinfo(res);
            }
            dbg(haveLan ? "[Lobby] no default IPv4 route; LAN address taken from this PC's own address list\n"
                        : "[Lobby] no default IPv4 route and no usable local IPv4 address\n");
        }
        if (haveLan) {
            char code[15] = {0}; LobbyCode::Encode(lan, (uint16_t)bindPort, code);
            _snprintf(m, sizeof(m), "[Lobby] host LAN code %s (%u.%u.%u.%u:%d) - same network only\n", code,
                      lan >> 24, (lan >> 16) & 0xFF, (lan >> 8) & 0xFF, lan & 0xFF, bindPort);
            m[sizeof(m) - 1] = 0; dbg(m);
            SetHostCode(code);   // until the host thread finds an internet code
        } else {
            dbg("[Lobby] host LAN address not found (no IPv4 route) - no LAN code\n");
        }
        char loop[15] = {0}; LobbyCode::Encode(0x7F000001u, (uint16_t)bindPort, loop);
        _snprintf(m, sizeof(m), "[Lobby] host same-PC code %s (127.0.0.1:%d)\n", loop, bindPort);
        m[sizeof(m) - 1] = 0; dbg(m);
        g_bindPort = bindPort;
        _beginthreadex(nullptr, 0, HostThread, nullptr, 0, nullptr);   // relay + UPnP + STUN, off the game thread
    }
}

namespace Net {

bool BeginJoin(uint32_t ip, uint16_t port, char* err, size_t errN);   // below Init

bool Init() {
    if (GetPrivateProfileIntA("Net", "Enabled", 0, Paths::Ini()) == 0) return false;

    int localPort = GetPrivateProfileIntA("Net", "LocalPort", 47801, Paths::Ini());
    int peerPort  = GetPrivateProfileIntA("Net", "PeerPort",  47801, Paths::Ini());
    char peerIp[64] = {0};
    GetPrivateProfileStringA("Net", "PeerIP", "127.0.0.1", peerIp, sizeof(peerIp), Paths::Ini());

    // [Net] Mode=manual|host|join (default manual = everything below behaves exactly as before).
    char modeStr[16] = {0}, joinCode[64] = {0};
    GetPrivateProfileStringA("Net", "Mode", "manual", modeStr, sizeof(modeStr), Paths::Ini());
    GetPrivateProfileStringA("Net", "JoinCode", "", joinCode, sizeof(joinCode), Paths::Ini());
    g_mode = LM_MANUAL;
    if (_stricmp(modeStr, "host") == 0) g_mode = LM_HOST;
    else if (_stricmp(modeStr, "join") == 0) g_mode = LM_JOIN;
    else if (_stricmp(modeStr, "lobby") == 0) g_mode = LM_LOBBY;
    else if (_stricmp(modeStr, "manual") != 0) {
        char m[96]; _snprintf(m, sizeof(m), "[Lobby] unknown [Net] Mode=%s, using manual\n", modeStr); m[sizeof(m) - 1] = 0; dbg(m);
    }
    // Host mode only: [Net] Upnp=1 (default) and [Net] StunServer=stun.l.google.com:19302 (default; empty = off).
    g_upnpEnabled = GetPrivateProfileIntA("Net", "Upnp", 1, Paths::Ini());
    GetPrivateProfileStringA("Net", "StunServer", "stun.l.google.com:19302", g_stunServer, sizeof(g_stunServer), Paths::Ini());
    // Lobby modes: [Net] RelayServer=host[:port] (default empty = no relay, nothing below changes) and
    // [Net] RelayForce=1 (skip the direct attempt). docs/design/relay.md.
    GetPrivateProfileStringA("Net", "RelayServer", "", g_relayName, sizeof(g_relayName), Paths::Ini());
    g_relayForce = GetPrivateProfileIntA("Net", "RelayForce", 0, Paths::Ini());
    // Join mode: a valid JoinCode starts joining at once; an empty or invalid one leaves the link up and
    // idle until the player copies a code and presses F8 (Net::JoinByCode). Nothing is sent meanwhile.
    uint32_t joinIp = 0; uint16_t joinPort = 0;
    bool joinNow = false;
    if (g_mode == LM_JOIN) {
        joinNow = joinCode[0] && LobbyCode::Decode(joinCode, &joinIp, &joinPort);
        if (joinCode[0] && !joinNow) {
            char m[192]; _snprintf(m, sizeof(m), "[Lobby] JoinCode \"%s\" is invalid (typo?) - nothing sent; copy the host's code and press F8\n", joinCode);
            m[sizeof(m) - 1] = 0; dbg(m);
        }
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { dbg("[Net] WSAStartup failed\n"); return false; }

    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) { dbg("[Net] socket failed\n"); return false; }

    // Bind even if another socket already holds the port (a leftover/transient process from a
    // previous run or the launch chain). With the link started only on the first in-world frame,
    // the LIVE game is always the last to bind - and on Windows the last binder wins the incoming
    // datagrams - so the live game always receives, whatever else is holding the port.
    { BOOL reuse = TRUE; setsockopt(g_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)); }

    // Instance slot comes from the owned mutex marker FIRST (D-018). With SO_REUSEADDR below the
    // bind can never fail, so a bind failure alone can no longer tell the 2nd instance apart.
    g_secondInstance = DetectSecondInstance();

    // Bind LocalPort; if it's already taken (2nd instance on the same PC sharing this ini), bind
    // PeerPort instead and send to LocalPort. Two-PC setups use LocalPort==PeerPort and just bind.
    // Slot 1 keeps exactly this behaviour; slot 2 is decided by the marker above, and a bind failure
    // is kept only as a SECONDARY signal (it can still fire for other reasons).
    int bindPort = g_secondInstance ? peerPort  : localPort;
    int sendPort = g_secondInstance ? localPort : peerPort;
    // Join mode: the guest needs no fixed port (its own NAT mapping carries the replies). A same-PC
    // second instance binds an ephemeral port so it cannot steal the host's LocalPort datagrams
    // (SO_REUSEADDR below: the last binder of a shared port receives).
    if ((g_mode == LM_JOIN || g_mode == LM_LOBBY) && g_secondInstance) bindPort = 0;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons((u_short)bindPort);
    if (bind(g_sock, (sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
        bindPort = g_secondInstance ? localPort : peerPort;
        sendPort = g_secondInstance ? peerPort : localPort;
        g_secondInstance = true;   // bind failure (secondary signal) -> this is the same-PC 2nd instance (guest)
        local.sin_port = htons((u_short)bindPort);
        if (bind(g_sock, (sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
            char m[96]; wsprintfA(m, "[Net] bind(%d/%d) failed err=%d\n", localPort, peerPort, WSAGetLastError()); dbg(m);
            closesocket(g_sock); g_sock = INVALID_SOCKET; return false;
        }
    }
    { char m[96]; wsprintfA(m, "[Net] instance slot=%d bind=%d peer=%d\n", g_secondInstance ? 2 : 1, bindPort, sendPort); dbg(m); }

    if (bindPort == 0) {   // ephemeral: log the port the OS picked
        sockaddr_in got{}; int gl = sizeof(got);
        if (getsockname(g_sock, (sockaddr*)&got, &gl) == 0) bindPort = ntohs(got.sin_port);
    }

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    if (g_mode == LM_MANUAL) {
        peer.sin_port = htons((u_short)sendPort);
        inet_pton(AF_INET, peerIp, &peer.sin_addr);
        g_peerReady = 1;
    } else {   // host: no peer until the first HELLO; join: no peer until StartJoin below / F8
        g_peerReady = 0;
    }
    PeerSet(peer);

    InitializeCriticalSection(&g_lock);

    g_enabled = true;
    g_lastRecvMs = 0;   // no packet accepted yet
    SelfTestWire();     // [Phase 2] prove the compact codec round-trips before any traffic
    { char lm[192]; LobbyCode::SelfTest(lm, sizeof(lm)); dbg(lm); }   // lobby-code format (slice 2): log only, no behaviour
    if (g_mode != LM_MANUAL) SelfTestLobby();
    _beginthreadex(nullptr, 0, (unsigned (__stdcall*)(void*))RecvThread, nullptr, 0, nullptr);   // after the self-test (it resets the rx shadow)
    char m[192];
    if (g_mode == LM_MANUAL) {
        wsprintfA(m, "[Net] up: bound:%d -> %s:%d\n", bindPort, peerIp, sendPort); dbg(m);
    } else if (g_mode == LM_HOST) {
        wsprintfA(m, "[Net] up: bound:%d mode=host (waiting for a guest)\n", bindPort); dbg(m);
        BeginHosting(bindPort);
    } else if (g_mode == LM_LOBBY) {
        wsprintfA(m, "[Net] up: bound:%d mode=lobby%s%s\n", bindPort, g_relayName[0] ? " relay=" : "", g_relayName); dbg(m);
        g_bindPort = bindPort;
        dbg("[Lobby] lobby mode: choose Host or Join in the AC4Coop panel (F6 shows it)\n");
    } else {
        wsprintfA(m, "[Net] up: bound:%d mode=join%s%s\n", bindPort, g_relayName[0] ? " relay=" : "", g_relayName); dbg(m);
        g_bindPort = bindPort;
        char err[160] = {0};
        if (joinNow && !BeginJoin(joinIp, joinPort, err, sizeof(err))) {
            _snprintf(m, sizeof(m), "[Lobby] JoinCode %s not used: %s\n", joinCode, err); m[sizeof(m) - 1] = 0; dbg(m);
        } else if (!joinNow) dbg("[Lobby] join mode idle: copy the host's code and press F8 in the game\n");
    }
    return true;
}

// A decoded code -> a direct join, or a relay room join (room codes: first octet 255, start with 'Z').
bool BeginJoin(uint32_t ip, uint16_t port, char* err, size_t errN) {
    char canon[15] = {0}; LobbyCode::Encode(ip, port, canon);
    if (Relay::IsRoomAddress(ip)) {
        if (!g_relayName[0]) {
            if (err && errN) lstrcpynA(err, "a room code (Z...) needs [Net] RelayServer= in AC4Coop.ini (the host's relay server)", (int)errN);
            return false;
        }
        StartRelayJoin(Relay::RoomFromAddress(ip, port), canon);
        return true;
    }
    StartJoin(ip, port, canon);
    return true;
}

int LobbyMode() { return (int)g_mode; }

void Log(const char* line) { dbg(line); }

bool JoinByCode(const char* text, char* err, size_t errN) {
    auto fail = [&](const char* why) { if (err && errN) { lstrcpynA(err, why, (int)errN); } return false; };
    if (!g_enabled) return fail("network link is off");
    if (g_mode == LM_HOST) return fail("this game is hosting; restart the game to join someone instead");
    if (g_mode != LM_JOIN && g_mode != LM_LOBBY) return fail("joining needs [Net] Mode=lobby (or join) in AC4Coop.ini");
    if (InterlockedCompareExchange(&g_joinState, 0, 0) == 2) return fail("already connected to a host");
    uint32_t ip = 0; uint16_t port = 0;
    if (!text || !LobbyCode::Decode(text, &ip, &port)) return fail("that is not a valid code (12 characters, XXXX-XXXX-XXXX)");
    if (g_mode == LM_LOBBY) { g_mode = LM_JOIN; MemoryBarrier(); dbg("[Lobby] panel: joining\n"); }
    return BeginJoin(ip, port, err, errN);
}

bool StartHosting(char* err, size_t errN) {
    auto fail = [&](const char* why) { if (err && errN) { lstrcpynA(err, why, (int)errN); } return false; };
    if (!g_enabled) return fail("network link is off");
    if (g_mode == LM_HOST) return true;
    if (g_mode == LM_JOIN) return fail("this game is joining; restart the game to host instead");
    if (g_mode != LM_LOBBY) return fail("hosting needs [Net] Mode=lobby (or host) in AC4Coop.ini");
    g_mode = LM_HOST; MemoryBarrier();
    char m[96]; wsprintfA(m, "[Lobby] panel: hosting on port %d\n", g_bindPort); dbg(m);
    BeginHosting(g_bindPort);
    return true;
}

bool HostCode(char out[16]) {
    AcquireSRWLockShared(&g_codeLock);
    lstrcpynA(out, g_hostCode, 16);
    ReleaseSRWLockShared(&g_codeLock);
    return g_mode == LM_HOST && out[0] != 0;
}

void LobbyStatus(char* out, size_t n) {
    if (!out || !n) return;
    out[0] = 0;
    if (!g_enabled || g_mode == LM_MANUAL) return;
    if (g_mode == LM_LOBBY) { _snprintf(out, n, "choose Host or Join (F6: panel)"); out[n - 1] = 0; return; }
    char code[16];
    AcquireSRWLockShared(&g_codeLock);
    lstrcpynA(code, g_mode == LM_HOST ? g_hostCode : g_joinCodeText, sizeof(code));
    ReleaseSRWLockShared(&g_codeLock);
    const bool ready = InterlockedCompareExchange(&g_peerReady, 0, 0) != 0;
    const LONG via = InterlockedCompareExchange(&g_linkVia, 0, 0);
    const char* viaTxt = via == 2 ? " (relay)" : via == 1 ? " (direct)" : "";
    if (g_mode == LM_HOST) {
        const char* note = (const char*)InterlockedCompareExchangePointer((PVOID volatile*)&g_hostNote, nullptr, nullptr);
        if (!code[0] && InterlockedCompareExchange(&g_hostDone, 0, 0) && note && note[0])
            _snprintf(out, n, "no code: %s", note);
        else
            _snprintf(out, n, "host code %s (F7 copies) | %s%s", code[0] ? code : "...", ready ? "guest joined" : "waiting for a guest",
                      ready ? viaTxt : "");
    } else {
        const LONG st = InterlockedCompareExchange(&g_joinState, 0, 0);
        char conn[32]; _snprintf(conn, sizeof(conn), "connected%s", viaTxt); conn[sizeof(conn) - 1] = 0;
        const char* s = st == 1 ? "joining..." : st == 2 ? conn : st == 3 ? "refused: different builds"
                      : st == 4 ? "no answer from host" : st == 5 ? RelayErrText(InterlockedCompareExchange(&g_relayErr, 0, 0))
                      : "copy a code, press F8 to join";
        if (code[0]) _snprintf(out, n, "join %s | %s", code, s);
        else _snprintf(out, n, "%s", s);
    }
    out[n - 1] = 0;
}

bool Enabled() { return g_enabled; }

bool IsSecondInstance() { return g_secondInstance; }

void Send(const PlayerState& s) {
    if (!g_enabled || InterlockedCompareExchange(&g_peerReady, 0, 0) == 0) return;   // lobby: no peer yet
    uint16_t slot = 0;
    const sockaddr_in peer = PeerCopy(&slot);
    SendRaw(&s, (int)sizeof(s), peer, slot);   // direct, or DATA through the relay
    InterlockedIncrement(&g_txPackets);
    InterlockedAdd(&g_txBytes, (LONG)sizeof(s));
}

bool GetRemote(PlayerState& out) {
    if (!g_enabled) return false;
    bool have;
    EnterCriticalSection(&g_lock);
    have = g_haveRemote;
    if (have) out = g_remote;
    LeaveCriticalSection(&g_lock);
    return have;
}

// Pop up to `max` queued player packets, oldest first. Returns how many were written to `out`.
// GetRemote is unchanged: callers that only want the newest packet keep using it.
int DrainRemote(PlayerState* out, int max) {
    if (!g_enabled || !out || max <= 0) return 0;
    int n = 0;
    EnterCriticalSection(&g_lock);
    while (n < max && g_rxqCount > 0) {
        int idx = (g_rxqHead - g_rxqCount + NET_RXQ) % NET_RXQ;   // oldest entry in the ring
        out[n++] = g_rxq[idx];
        --g_rxqCount;
    }
    LeaveCriticalSection(&g_lock);
    return n;
}

void SendEntities(const EntityPacket& p) {
    // Lobby: no peer yet -> send nothing, and do not encode either (the encoder advances the delta shadow).
    if (!g_enabled || InterlockedCompareExchange(&g_peerReady, 0, 0) == 0) return;
    uint16_t slot = 0;
    const sockaddr_in peer = PeerCopy(&slot);
    const size_t n = EncodeEntityPacket(p, g_txBuf, sizeof(g_txBuf));   // [header][records]
    if (n == 0) return;
    const size_t hdr = sizeof(EntityWireHeader);
    const size_t recordBytes = n - hdr;
    // X15: chunk the record region so every UDP payload stays <= ENTITY_CHUNK_MAX. A header-only
    // snapshot (recordBytes == 0) still sends one chunk so the peer sees the heartbeat.
    const uint16_t total = (recordBytes == 0) ? 1u
                            : (uint16_t)((recordBytes + ENTITY_CHUNK_DATA - 1) / ENTITY_CHUNK_DATA);
    uint8_t chunk[ENTITY_CHUNK_MAX];
    for (uint16_t i = 0; i < total; ++i) {
        const size_t off = (size_t)i * ENTITY_CHUNK_DATA;
        size_t len = recordBytes - off;
        if (len > (size_t)ENTITY_CHUNK_DATA) len = (size_t)ENTITY_CHUNK_DATA;
        EntityWireHeader h{};
        std::memcpy(&h, g_txBuf, sizeof(h));   // magic/proto/seq/epoch/count/worldId already set
        h.chunkIndex = i;
        h.chunkTotal = total;
        std::memcpy(chunk, &h, sizeof(h));
        std::memcpy(chunk + hdr, g_txBuf + hdr + off, len);
        SendRaw(chunk, (int)(hdr + len), peer, slot);   // <= 1400 B + 16 B relay header <= Relay::kMaxDatagram
        InterlockedIncrement(&g_txPackets);
        InterlockedAdd(&g_txBytes, (LONG)(hdr + len));   // actual bytes on the wire (per chunk)
    }
    InterlockedAdd(&g_txEnts, (LONG)g_wireRecs);
    static uint32_t s_logCounter = 0;
    if ((++s_logCounter % 60) == 0) {
        char m[160];
        wsprintfA(m, "[Net] entwire pkt=%u B chunks=%u sent=%u/%u full=%u/%u\n",
                  (unsigned)n, (unsigned)total, (unsigned)g_wireRecs, (unsigned)p.count,
                  (unsigned)g_wireFullRecs, (unsigned)g_wireRecs);
        dbg(m);
    }
}

bool GetEntities(EntityPacket& out) {
    if (!g_enabled) return false;
    bool have;
    EnterCriticalSection(&g_lock);
    // The snapshot slot is only ever overwritten, never cleared, so a peer that dies mid-session
    // would have its last snapshot re-applied by the frame forever. Expire it after
    // NET_ENTITIES_TTL_MS with no snapshot write; the world then falls back to no-drive.
    if (g_haveEntities && g_entitiesMs != 0 &&
        (GetTickCount64() - g_entitiesMs) > NET_ENTITIES_TTL_MS) {
        g_haveEntities = false;
        std::memset(&g_remoteEntities, 0, sizeof(g_remoteEntities));
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            dbg("[Net] entity snapshot expired: no update for 3 s, dropping peer snapshot\n");
        }
    }
    have = g_haveEntities;
    if (have) out = g_remoteEntities;
    LeaveCriticalSection(&g_lock);
    return have;
}

// B-4: drain the received DEATH ring into `out` (n = number of entries copied, 0 = nothing pending).
// The packet is shaped like any other EntityPacket, so the caller hands it straight to
// ProcessKillEvents. This is what makes a one-entry KIND_KILL datagram survive a snapshot arriving in
// the same frame gap: GetEntities() only ever exposes the latest snapshot, this queue keeps the deaths.
uint32_t GetKillEntities(EntityPacket& out, uint32_t max) {
    if (!g_enabled) return 0;
    if (max == 0 || max > (uint32_t)MAX_SYNC_ENTITIES) max = (uint32_t)MAX_SYNC_ENTITIES;
    out.magic = NET_ENTITY_MAGIC;
    out.protoVer = NET_PROTO_VER;
    out.seq = 0;
    out.count = 0;
    uint32_t n = 0;
    EnterCriticalSection(&g_lock);
    while (n < max && g_killCount > 0) {
        const int idx = (g_killHead - g_killCount + NET_KILLQ) % NET_KILLQ;   // oldest entry
        out.ents[n++] = g_killRing[idx];
        --g_killCount;
    }
    out.count = n;
    LeaveCriticalSection(&g_lock);
    if (n) out.seq = GetTickCount();
    return n;
}

// ---- session-lifecycle hardening (Feature #3) --------------------------------------
void SetStrictPeer(bool strict) { g_strictPeer = strict; }

// X5: the local world id (World::GetObjectID()). The entity encoder stamps it into the wire header;
// the decoder drops snapshots whose world differs from the local one. Interlocked 64-bit write so the
// recv thread's read (ReadLocalWorldId) never tears on this x86 build.
void SetLocalWorldId(uint64_t id) {
    // X5: a world CHANGE re-arms the bootstrap allowance. Without this the "confirmed" flag stayed
    // latched for the life of the process, so the first legitimate worldId == 0 frame of a NEW world
    // was DEC_WORLD forever. Only an actual change resets it - phase1 calls this every frame with the
    // same id. Interlocked because the decoder reads it on the recv thread.
    if ((uint64_t)InterlockedCompareExchange64(&g_localWorldId, 0, 0) != id)
        InterlockedExchange(&g_worldConfirmed, 0);
    InterlockedExchange64(&g_localWorldId, (LONGLONG)id);
}

uint64_t LastRecvMs() {
    if (!g_enabled) return 0;
    EnterCriticalSection(&g_lock);
    uint64_t t = g_lastRecvMs;
    LeaveCriticalSection(&g_lock);
    return t;
}

uint32_t MillisSinceRecv() {
    if (!g_enabled) return 0xFFFFFFFFu;
    EnterCriticalSection(&g_lock);
    uint64_t t = g_lastRecvMs;
    LeaveCriticalSection(&g_lock);
    if (t == 0) return 0xFFFFFFFFu;               // nothing received yet
    return (uint32_t)(GetTickCount64() - t);       // 64-bit delta, narrowed after the subtraction
}

// ---- wire counters ------------------------------------------------------------------------------
void GetStats(Stats& out) {
    out.txPackets = (uint32_t)InterlockedCompareExchange(&g_txPackets, 0, 0);
    out.txBytes   = (uint32_t)InterlockedCompareExchange(&g_txBytes,   0, 0);
    out.txEnts    = (uint32_t)InterlockedCompareExchange(&g_txEnts,    0, 0);
    out.rxPackets = (uint32_t)InterlockedCompareExchange(&g_rxPackets, 0, 0);
    out.rxBytes   = (uint32_t)InterlockedCompareExchange(&g_rxBytes,   0, 0);
    out.rejects       = (uint32_t)InterlockedCompareExchange(&g_rxRejects,       0, 0);
    out.protoMismatch = (uint32_t)InterlockedCompareExchange(&g_rxProtoMismatch, 0, 0);
    out.sizeMismatch  = (uint32_t)InterlockedCompareExchange(&g_rxSizeMismatch,  0, 0);
    out.staleChunks   = (uint32_t)InterlockedCompareExchange(&g_rxStaleChunks,   0, 0);
}

void ResetStats() {
    InterlockedExchange(&g_txPackets, 0);
    InterlockedExchange(&g_txBytes,   0);
    InterlockedExchange(&g_txEnts,    0);
    InterlockedExchange(&g_rxPackets, 0);
    InterlockedExchange(&g_rxBytes,   0);
    InterlockedExchange(&g_rxRejects,       0);
    InterlockedExchange(&g_rxProtoMismatch, 0);
    InterlockedExchange(&g_rxSizeMismatch,  0);
    InterlockedExchange(&g_rxStaleChunks,   0);
}

} // namespace Net
