// [Coop] PoseSync (default 0) / PoseSyncApply (default 0): F3 pose parity, docs/design/pose-sync-impl.md.
//
// AC4BFSP.exe md5 2058342866688f780c8b34526a65bc35, IDA session 4b0f0b4a, 2026-11-27 (re-verified for this file):
//   sub_453740 = SkeletonComponent vtable slot 44 (dword 0x01E41F08 = 0x00453740; vtable 0x01E41E58 + 44*4):
//     0x45374D lea ecx,[esi+104h]; call sub_D587F0 (component lock) ... 0x453763 call sub_453330 ... 0x453775 release.
//   sub_453330 (only caller 0x453763): 0x453331 mov esi,ecx and ESI is never written again in the body; the animation
//     graph (0x453392 sub_6934B0, 0x4533C2 sub_5757C0, 0x453401 sub_583730) and the IK write sub_44FB70 (0x453435) run
//     on LOCAL first, then
//       0x453488 mov eax,[esi+138h]   current WORLD
//       0x45348E mov ecx,[esi+134h]   LOCAL
//       0x45349A push edx (LOD level) ; 0x45349B push eax
//       0x45349C call sub_769150      bytes E8 AF 5C 31 00 (-> 0x4534A1 + 0x315CAF = 0x769150)
//       0x4534A1 pop edi ; pop esi ; retn
//     sub_769150 (__thiscall this=src LOCAL, dst, level; retn 8) composes LOCAL -> WORLD.
//   So at 0x45349C, ESI = the SkeletonComponent, ECX = its LOCAL pose, and LOCAL holds this tick's animation + IK.
//   We redirect that call to ComposeThunk: pushad, handler(ESI, ECX), popad, jmp 0x769150 (return address 0x4534A1
//   and both arguments stay on the stack exactly as the original call left them; sub_769150 pops them, retn 8).
//
//   Bone lookup sub_44DB80: __thiscall ECX = comp, one stack arg (crc32), retn 4, result AL; 0x44DB84 mov ecx,[ecx+128h]
//     (null map -> miss), lower_bound over 8-byte {u32 hash, u8 idx} entries at desc+0x28, count u16 desc+0x2E
//     (0x44DB8E/0x44DB91), miss -> byte_1E70038 = 0xFF. Calls only sub_1C67DF0 (32 insns, no call: leaf). No lock.
//   Pose accessors (this = pose, arg0 = bone index (dword, used as [table+idx*2]), arg1 = float4*; retn 8):
//     sub_758C30 get rotation  - output written with movaps [eax] (0x758D2E, 0x758D64, 0x758E3E): 16-byte aligned.
//     sub_75A2E0 get translation - movaps [esi] (0x75A36B, 0x75A39F): aligned.
//     sub_759060 set rotation  - input read with movq (0x75907C): any alignment.
//     sub_759250 set translation - movq (0x75926C).
//     All four apply the pose's root transform only when pose+0x8C bit 27 is clear (0x758D6D shr ecx,1Bh; test cl,1;
//     jnz skip). We require the bit SET (LOCAL) and otherwise touch nothing, so the root path (sub_A4A4E0/sub_4A13D0,
//     the only calls these accessors make) is never entered. None takes a lock.
#include "pose_sync.h"
#include "Hook.h"
#include "paths.h"
#include "net.h"
#include "netstate.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <algorithm>

void Phase1_LogLine(const char* s);

namespace {

const uint32_t VT_SKELETON_COMPONENT = 0x01E41E58;
const uintptr_t VA_SITE    = 0x0045349A;       // push edx ; push eax ; call sub_769150 ; pop edi ; pop esi ; retn
const uintptr_t VA_COMPOSE = 0x00769150;
const BYTE kSite[10] = { 0x52, 0x50, 0xE8, 0xAF, 0x5C, 0x31, 0x00, 0x5F, 0x5E, 0xC3 };

struct BoneDef { const char* name; uint8_t trans; };
// HumanIK names. All but Hips are entries of the engine's own name table at 0x1E70040 (26 dwords, decoded with
// zlib.crc32 2026-11-27: Reference Spine Spine1 Spine2 Neck Head LeftUpLeg LeftLeg LeftFoot LeftFootThumb1
// LeftClavicle LeftArm LeftForeArm LeftHand LeftHandMiddle1 ? RightUpLeg RightLeg RightFoot RightFootThumb1
// RightClavicle RightArm RightForeArm RightHand RightHandMiddle1 ?). Hips = 0xDED10611 (ac4-bone-name-map.md).
// Whether each resolves on the live player rig is logged ([Pose] tx rig ... missing=[...]), not assumed.
// Reference (the root, 0x2C52CBB0) is deliberately NOT sent: what its LOCAL record holds relative to the entity
// transform that ApplyRemoteState already drives is UNVERIFIED, and a wrong root would move the whole body.
// Translation is sent for Hips only (pelvis height / sway); every other bone keeps its own rig's bone length.
const BoneDef kBones[] = {
    { "Hips", 1 }, { "Spine", 0 }, { "Spine1", 0 }, { "Spine2", 0 }, { "Neck", 0 }, { "Head", 0 },
    { "LeftClavicle", 0 },  { "LeftArm", 0 },  { "LeftForeArm", 0 },  { "LeftHand", 0 },  { "LeftHandMiddle1", 0 },
    { "RightClavicle", 0 }, { "RightArm", 0 }, { "RightForeArm", 0 }, { "RightHand", 0 }, { "RightHandMiddle1", 0 },
    { "LeftUpLeg", 0 },  { "LeftLeg", 0 },  { "LeftFoot", 0 },  { "LeftFootThumb1", 0 },
    { "RightUpLeg", 0 }, { "RightLeg", 0 }, { "RightFoot", 0 }, { "RightFootThumb1", 0 },
};
const int NB = (int)(sizeof(kBones) / sizeof(kBones[0]));
static_assert(NB <= 32 && NB <= POSE_RECORDS_MAX, "bone mask is a u32 and one datagram");
uint32_t g_hash[NB];

// ---- config ----
int  g_on = 0, g_apply = 0, g_applyTrans = 1, g_hz = 10, g_blendPct = 100, g_maxAgeMs = 500, g_loopback = 0;
DWORD g_composeFn = 0;
char g_initMsg[200] = "";

// ---- shared with the hook (any thread) ----
void* volatile g_capComp = nullptr;  void* volatile g_capOwner = nullptr;
void* volatile g_appComp = nullptr;  void* volatile g_appOwner = nullptr;
volatile LONG  g_capWant = 0;
CRITICAL_SECTION g_capLock, g_rxLock;

struct Capture {
    uint32_t tick;            // +1 per capture
    uint32_t hostMs;
    uint32_t mode;            // LOCAL pose+0x90 (0 float, 1 packed)
    uint32_t mask;            // bit i = kBones[i] resolved
    int      n;
    PoseWireRecord rec[NB];
} g_cap = {};

struct Rx {
    uint32_t seq;
    uint64_t rxMs;
    int      n;
    uint32_t hash[POSE_RECORDS_MAX];
    uint8_t  flags[POSE_RECORDS_MAX];
    float    q[POSE_RECORDS_MAX][4];
    float    t[POSE_RECORDS_MAX][4];
} g_rx = {};

const int ERR_MAX = 4096;
struct Metric {               // guarded by g_rxLock
    uint32_t seq;             // last seq measured (one sample set per received frame)
    uint32_t matched, total;  // of the last measured frame
    uint32_t unmatched[8]; int nUnmatched;
    int   n;
    float errOut[ERR_MAX];    // angle(LOCAL as handed to the compose, received)  - after the apply, if any
    float errNat[ERR_MAX];    // angle(puppet's own animated LOCAL, received)       - before the apply
} g_m = {};

volatile LONG c_capOk = 0, c_capBusy = 0, c_rootCap = 0, c_rootApp = 0, c_applyBusy = 0, c_appliedTicks = 0,
              c_appliedBones = 0, c_clampT = 0, c_old = 0, c_exc = 0, c_ownerMiss = 0, c_capHook = 0, c_appHook = 0;

// ---- engine calls (no locks; see the header comment) ----
inline uint8_t BoneIndex(void* comp, uint32_t hash) {
    return ((uint8_t(__thiscall*)(void*, uint32_t))0x0044DB80)(comp, hash);
}
inline void GetRot(void* pose, int idx, float* q16) { ((void(__thiscall*)(void*, int, float*))0x00758C30)(pose, idx, q16); }
inline void GetTrans(void* pose, int idx, float* t16) { ((void(__thiscall*)(void*, int, float*))0x0075A2E0)(pose, idx, t16); }
inline void SetRot(void* pose, int idx, const float* q) { ((void(__thiscall*)(void*, int, const float*))0x00759060)(pose, idx, q); }
inline void SetTrans(void* pose, int idx, const float* t) { ((void(__thiscall*)(void*, int, const float*))0x00759250)(pose, idx, t); }

uint32_t Crc32(const char* s) {
    uint32_t c = 0xFFFFFFFFu;
    for (; *s; ++s) { c ^= (uint8_t)*s; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
    return ~c;
}
uint32_t QpcMs() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint32_t)((uint64_t)(c.QuadPart / f.QuadPart) * 1000ull + (uint64_t)((c.QuadPart % f.QuadPart) * 1000ull / f.QuadPart));
}

// ---- quaternion helpers ----
void QNorm(float* q) {
    const float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (l < 1e-8f) { q[0] = q[1] = q[2] = 0; q[3] = 1; return; }
    for (int i = 0; i < 4; ++i) q[i] /= l;
}
float QAngleDeg(const float* a, const float* b) {   // double: float acos near 1 alone is worth ~0.04 deg
    const double la = std::sqrt((double)a[0] * a[0] + (double)a[1] * a[1] + (double)a[2] * a[2] + (double)a[3] * a[3]);
    const double lb = std::sqrt((double)b[0] * b[0] + (double)b[1] * b[1] + (double)b[2] * b[2] + (double)b[3] * b[3]);
    if (la < 1e-9 || lb < 1e-9) return 180.f;
    double d = std::fabs(((double)a[0] * b[0] + (double)a[1] * b[1] + (double)a[2] * b[2] + (double)a[3] * b[3]) / (la * lb));
    if (d > 1.0) d = 1.0;
    return (float)(2.0 * std::acos(d) * 57.295779513082);
}
void QSlerp(const float* a, const float* b0, float t, float* out) {
    float b[4] = { b0[0], b0[1], b0[2], b0[3] };
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0) { d = -d; for (int i = 0; i < 4; ++i) b[i] = -b[i]; }
    float wa, wb;
    if (d > 0.9995f) { wa = 1.f - t; wb = t; }
    else { const float th = std::acos(d), s = std::sin(th); wa = std::sin((1.f - t) * th) / s; wb = std::sin(t * th) / s; }
    for (int i = 0; i < 4; ++i) out[i] = wa * a[i] + wb * b[i];
    QNorm(out);
}
const float kInvSqrt2 = 0.70710678f;
void EncodeQ48(const float* q0, uint8_t* o) {
    float q[4] = { q0[0], q0[1], q0[2], q0[3] }; QNorm(q);
    int m = 0; for (int i = 1; i < 4; ++i) if (std::fabs(q[i]) > std::fabs(q[m])) m = i;
    if (q[m] < 0) for (int i = 0; i < 4; ++i) q[i] = -q[i];
    uint64_t bits = (uint64_t)m; int sh = 2;
    for (int i = 0; i < 4; ++i) {
        if (i == m) continue;
        float v = (q[i] / kInvSqrt2) * 0.5f + 0.5f; v = v < 0 ? 0 : (v > 1 ? 1 : v);
        bits |= (uint64_t)(uint32_t)std::lround(v * 32767.f) << sh; sh += 15;
    }
    for (int i = 0; i < 6; ++i) o[i] = (uint8_t)(bits >> (8 * i));
}
void DecodeQ48(const uint8_t* in, float* q) {
    uint64_t bits = 0; for (int i = 0; i < 6; ++i) bits |= (uint64_t)in[i] << (8 * i);
    const int m = (int)(bits & 3); int sh = 2; float ss = 0;
    for (int i = 0; i < 4; ++i) {
        if (i == m) continue;
        const float v = (float)((bits >> sh) & 0x7FFF) / 32767.f; sh += 15;
        q[i] = (v - 0.5f) * 2.f * kInvSqrt2; ss += q[i] * q[i];
    }
    q[m] = std::sqrt(ss < 1.f ? 1.f - ss : 0.f);
    QNorm(q);
}
int16_t QuantT(float v) {
    float s = v * 2048.f;
    if (s > 32767.f) { s = 32767.f; InterlockedIncrement(&c_clampT); }
    if (s < -32768.f) { s = -32768.f; InterlockedIncrement(&c_clampT); }
    return (int16_t)std::lround(s);
}

// ---- hook side (whatever thread runs SkeletonComponent slot 44; the engine holds comp+0x104) ----
bool PoseHeaderOk(uint8_t* comp, uint8_t* local, void* owner) {
    if (*(void**)(comp + 0x8) != owner || *(uint8_t**)(comp + 0x134) != local) { InterlockedIncrement(&c_ownerMiss); return false; }
    return true;
}

void CaptureLocked(uint8_t* comp, uint8_t* local) {   // no SEH here: aligned locals; called under __try
    __declspec(align(16)) float q[4];
    __declspec(align(16)) float t[4];
    int n = 0; uint32_t mask = 0;
    for (int i = 0; i < NB; ++i) {
        const uint8_t idx = BoneIndex(comp, g_hash[i]);   // every tick: never cache an index across ticks or rigs
        if (idx == 0xFF) continue;
        mask |= 1u << i;
        PoseWireRecord& r = g_cap.rec[n++];
        std::memset(&r, 0, sizeof(r));
        r.hash = g_hash[i];
        GetRot(local, idx, q);
        EncodeQ48(q, r.q48);
        if (kBones[i].trans) {
            GetTrans(local, idx, t);
            r.flags = POSE_RF_TRANS;
            r.t[0] = QuantT(t[0]); r.t[1] = QuantT(t[1]); r.t[2] = QuantT(t[2]);
        }
    }
    g_cap.n = n; g_cap.mask = mask; g_cap.mode = *(uint32_t*)(local + 0x90);
    g_cap.hostMs = QpcMs(); ++g_cap.tick;
}

void DoCapture(uint8_t* comp, uint8_t* local) {
    if (InterlockedCompareExchange(&g_capWant, 0, 0) == 0) return;
    __try {
        if (!PoseHeaderOk(comp, local, g_capOwner)) return;
        if ((*(uint32_t*)(local + 0x8C) & 0x08000000u) == 0) { InterlockedIncrement(&c_rootCap); return; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&c_exc); return; }
    if (!TryEnterCriticalSection(&g_capLock)) { InterlockedIncrement(&c_capBusy); return; }
    __try { CaptureLocked(comp, local); InterlockedIncrement(&c_capOk); }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&c_exc); }
    LeaveCriticalSection(&g_capLock);
    InterlockedExchange(&g_capWant, 0);
}

void ApplyLocked(uint8_t* comp, uint8_t* local) {     // g_rxLock held; no SEH here
    if (g_rx.n == 0) return;
    if (GetTickCount64() - g_rx.rxMs > (uint64_t)g_maxAgeMs) { InterlockedIncrement(&c_old); return; }
    __declspec(align(16)) float qn[4];
    __declspec(align(16)) float qo[4];
    __declspec(align(16)) float tn[4];
    __declspec(align(16)) float to[4];
    const bool metric = (g_rx.seq != g_m.seq);
    const float a = (float)g_blendPct / 100.f;
    uint32_t matched = 0; int nUn = 0; uint32_t un[8] = {};
    for (int i = 0; i < g_rx.n; ++i) {
        const uint8_t idx = BoneIndex(comp, g_rx.hash[i]);   // resolved on the puppet's own rig, every tick
        if (idx == 0xFF) { if (nUn < 8) un[nUn++] = g_rx.hash[i]; continue; }
        ++matched;
        GetRot(local, idx, qn);
        const float eNat = QAngleDeg(qn, g_rx.q[i]);
        float eOut = eNat;
        if (g_apply) {
            QSlerp(qn, g_rx.q[i], a, qo);
            SetRot(local, idx, qo);
            if ((g_rx.flags[i] & POSE_RF_TRANS) && g_applyTrans) {
                GetTrans(local, idx, tn);
                for (int k = 0; k < 3; ++k) to[k] = tn[k] + (g_rx.t[i][k] - tn[k]) * a;
                to[3] = 0.f;
                SetTrans(local, idx, to);
            }
            GetRot(local, idx, qo);                           // read back what the compose will consume
            eOut = QAngleDeg(qo, g_rx.q[i]);
            InterlockedIncrement(&c_appliedBones);
        }
        if (metric && g_m.n < ERR_MAX) { g_m.errOut[g_m.n] = eOut; g_m.errNat[g_m.n] = eNat; ++g_m.n; }
    }
    if (metric) {
        g_m.seq = g_rx.seq; g_m.matched = matched; g_m.total = (uint32_t)g_rx.n;
        g_m.nUnmatched = nUn; std::memcpy(g_m.unmatched, un, sizeof(un));
    }
    if (g_apply) InterlockedIncrement(&c_appliedTicks);
}

void DoApply(uint8_t* comp, uint8_t* local) {
    __try {
        if (!PoseHeaderOk(comp, local, g_appOwner)) return;
        if ((*(uint32_t*)(local + 0x8C) & 0x08000000u) == 0) { InterlockedIncrement(&c_rootApp); return; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&c_exc); return; }
    if (!TryEnterCriticalSection(&g_rxLock)) { InterlockedIncrement(&c_applyBusy); return; }
    __try { ApplyLocked(comp, local); }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&c_exc); }
    LeaveCriticalSection(&g_rxLock);
}

void __cdecl OnCompose(uint8_t* comp, uint8_t* local) {
    if (!comp) return;
    if (comp == (uint8_t*)g_capComp) { InterlockedIncrement(&c_capHook); DoCapture(comp, local); return; }
    if (comp == (uint8_t*)g_appComp) { InterlockedIncrement(&c_appHook); DoApply(comp, local); }
}

// At 0x45349C: ESI = comp, ECX = LOCAL, [esp] = 0x4534A1 (pushed by our call), [esp+4] = WORLD, [esp+8] = level.
__declspec(naked) void ComposeThunk() {
    __asm {
        pushad
        push ecx
        push esi
        call OnCompose
        add  esp, 8
        popad
        jmp  dword ptr [g_composeFn]
    }
}

// ---- main thread ----
void* FindSkel(void* e) {
    if (!e) return nullptr;
    __try {
        const uint16_t n = *(uint16_t*)((uint8_t*)e + 0x66);
        void** arr = *(void***)((uint8_t*)e + 0x60);
        if (n == 0 || n > 96 || !arr) return nullptr;
        for (uint16_t i = 0; i < n; ++i)
            if (arr[i] && *(uint32_t*)arr[i] == VT_SKELETON_COMPONENT) return arr[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

void Ingest(const uint8_t* b, int n, uint64_t rxMs) {
    if (n < (int)sizeof(PoseWireHeader)) return;
    PoseWireHeader h; std::memcpy(&h, b, sizeof(h));
    if (h.count > POSE_RECORDS_MAX || n != (int)sizeof(h) + h.count * (int)sizeof(PoseWireRecord)) return;
    static Rx tmp;   // main thread only
    tmp.seq = h.seq; tmp.rxMs = rxMs; tmp.n = h.count;
    for (int i = 0; i < h.count; ++i) {
        PoseWireRecord r; std::memcpy(&r, b + sizeof(h) + (size_t)i * sizeof(r), sizeof(r));
        tmp.hash[i] = r.hash; tmp.flags[i] = r.flags;
        DecodeQ48(r.q48, tmp.q[i]);
        for (int k = 0; k < 3; ++k) tmp.t[i][k] = (float)r.t[k] / 2048.f;
        tmp.t[i][3] = 0.f;
    }
    EnterCriticalSection(&g_rxLock);
    g_rx = tmp;
    LeaveCriticalSection(&g_rxLock);
}

float Pct(float* v, int n, float p) {
    if (n <= 0) return -1.f;
    std::sort(v, v + n);
    int i = (int)std::ceil(p * n) - 1; if (i < 0) i = 0; if (i >= n) i = n - 1;
    return v[i];
}
const char* BoneName(uint32_t h) {
    for (int i = 0; i < NB; ++i) if (g_hash[i] == h) return kBones[i].name;
    return nullptr;
}

}  // namespace

void PoseSync::Init()
{
    g_on = GetPrivateProfileIntA("Coop", "PoseSync", 0, Paths::Ini()) != 0;
    if (!g_on) return;                                        // flag 0: nothing patched, nothing logged
    g_apply      = GetPrivateProfileIntA("Coop", "PoseSyncApply", 0, Paths::Ini()) != 0;
    g_applyTrans = GetPrivateProfileIntA("Coop", "PoseSyncApplyTrans", 1, Paths::Ini()) != 0;
    g_hz         = (std::min)(60, (std::max)(1, (int)GetPrivateProfileIntA("Coop", "PoseSyncHz", 10, Paths::Ini())));
    g_blendPct   = (std::min)(100, (std::max)(0, (int)GetPrivateProfileIntA("Coop", "PoseSyncBlendPct", 100, Paths::Ini())));
    g_maxAgeMs   = (std::max)(50, (int)GetPrivateProfileIntA("Coop", "PoseSyncMaxAgeMs", 500, Paths::Ini()));
    g_loopback   = GetPrivateProfileIntA("Coop", "PoseSyncLoopback", 0, Paths::Ini()) != 0;
    for (int i = 0; i < NB; ++i) g_hash[i] = Crc32(kBones[i].name);
    // codec self-check (logged with the install line): smallest-three round trip on 2000 pseudo-random unit
    // quaternions, plus the crc32 positive control crc32("Head") == 0x07C159A2 (ac4-bone-name-map.md A2).
    float q48Max = 0.f; uint32_t rs = 0x12345678u;
    for (int k = 0; k < 2000; ++k) {
        float q[4], d[4]; uint8_t b[6];
        for (int i = 0; i < 4; ++i) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; q[i] = (float)(rs % 20001u) / 10000.f - 1.f; }
        QNorm(q); EncodeQ48(q, b); DecodeQ48(b, d);
        q48Max = (std::max)(q48Max, QAngleDeg(q, d));
    }
    const bool crcOk = Crc32("Head") == 0x07C159A2u;
    InitializeCriticalSection(&g_capLock);
    InitializeCriticalSection(&g_rxLock);

    BYTE* site = (BYTE*)(GameModuleBase + (VA_SITE - 0x00400000));
    if (memcmp(site, kSite, sizeof(kSite)) != 0) {
        _snprintf_s(g_initMsg, sizeof(g_initMsg), _TRUNCATE,
                    "[Pose] PoseSync=1 but bytes at 0x45349A differ - NOT installed (capture/apply off)\n");
        g_on = 0; return;
    }
    g_composeFn = (DWORD)(GameModuleBase + (VA_COMPOSE - 0x00400000));
    BYTE* call = site + 2;
    DWORD old = 0;
    if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old)) { g_on = 0; return; }
    const int32_t rel = (int32_t)((uintptr_t)&ComposeThunk - ((uintptr_t)call + 5));
    std::memcpy(call + 1, &rel, 4);
    VirtualProtect(call, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    _snprintf_s(g_initMsg, sizeof(g_initMsg), _TRUNCATE,
                "[Pose] PoseSync=1 installed at 0x45349C apply=%d trans=%d hz=%d blend=%d%% maxAge=%dms loopback=%d bones=%d "
                "q48maxErr=%.4fdeg crcHead=%s\n",
                g_apply, g_applyTrans, g_hz, g_blendPct, g_maxAgeMs, g_loopback, NB, q48Max, crcOk ? "ok" : "BAD");
}

void PoseSync::ForgetPuppet()
{
    g_appComp = nullptr;
    g_appOwner = nullptr;
}

void PoseSync::Tick(Entity* player, Entity* puppet)
{
    if (!g_on) return;
    static bool s_first = true;
    if (s_first) { s_first = false; Phase1_LogLine(g_initMsg); }

    // publish the two components (owner first: the hook gates on the component, then checks the owner)
    void* pc = FindSkel(player);
    void* ac = FindSkel(puppet);
    if (pc != g_capComp) { g_capComp = nullptr; g_capOwner = player; g_capComp = pc; }
    if (ac != g_appComp) { g_appComp = nullptr; g_appOwner = puppet; g_appComp = ac; }

    const uint32_t now = QpcMs();
    static uint32_t s_lastWant = 0, s_lastTick = 0, s_seq = 0, s_rxSeq = 0;
    static void* s_logRig = nullptr; static uint32_t s_mask = 0, s_mode = 0;
    if (pc && now - s_lastWant >= (uint32_t)(1000 / g_hz)) { s_lastWant = now; InterlockedExchange(&g_capWant, 1); }

    // send the newest capture
    static uint8_t dg[POSE_DGRAM_MAX];
    int len = 0;
    EnterCriticalSection(&g_capLock);
    if (g_cap.tick != s_lastTick && g_cap.n > 0) {
        s_lastTick = g_cap.tick; s_mask = g_cap.mask; s_mode = g_cap.mode;
        PoseWireHeader h = {};
        h.magic = NET_POSE_MAGIC; h.protoVer = NET_PROTO_VER; h.seq = ++s_seq; h.hostMs = g_cap.hostMs;
        h.kind = POSE_KIND_FRAME; h.count = (uint8_t)g_cap.n;
        std::memcpy(dg, &h, sizeof(h));
        std::memcpy(dg + sizeof(h), g_cap.rec, (size_t)g_cap.n * sizeof(PoseWireRecord));
        len = (int)sizeof(h) + g_cap.n * (int)sizeof(PoseWireRecord);
    }
    LeaveCriticalSection(&g_capLock);
    if (len > 0) {
        if (pc != s_logRig) {   // once per rig: which names resolved on the sender
            s_logRig = pc;
            char miss[300] = ""; size_t o = 0; int got = 0;
            for (int i = 0; i < NB; ++i) {
                if (s_mask & (1u << i)) { ++got; continue; }
                o += (size_t)_snprintf_s(miss + o, sizeof(miss) - o, _TRUNCATE, "%s%s", o ? "," : "", kBones[i].name);
                if (o >= sizeof(miss) - 1) break;
            }
            char m[420];
            _snprintf_s(m, sizeof(m), _TRUNCATE, "[Pose] tx rig comp=%p resolved=%d/%d mode=%u missing=[%s]\n", pc, got, NB, s_mode, miss);
            Phase1_LogLine(m);
        }
        Net::SendPose(dg, len);
        if (g_loopback) Ingest(dg, len, GetTickCount64());
    }

    // receive
    static uint8_t rb[POSE_DGRAM_MAX];
    uint64_t rxMs = 0;
    const int rn = Net::TakePose(rb, sizeof(rb), &s_rxSeq, &rxMs);
    if (rn > 0) Ingest(rb, rn, rxMs);

    // report every 5 s
    static uint64_t s_lastLog = 0;
    const uint64_t t64 = GetTickCount64();
    if (s_lastLog == 0) s_lastLog = t64;
    if (t64 - s_lastLog < 5000) return;
    s_lastLog = t64;
    static float eo[ERR_MAX], en[ERR_MAX];
    int n = 0; uint32_t matched = 0, total = 0; int nUn = 0; uint32_t un[8] = {};
    EnterCriticalSection(&g_rxLock);
    n = g_m.n; std::memcpy(eo, g_m.errOut, n * sizeof(float)); std::memcpy(en, g_m.errNat, n * sizeof(float));
    matched = g_m.matched; total = g_m.total; nUn = g_m.nUnmatched; std::memcpy(un, g_m.unmatched, sizeof(un));
    g_m.n = 0;
    LeaveCriticalSection(&g_rxLock);
    char unb[200] = ""; size_t o = 0;
    for (int i = 0; i < nUn && o < sizeof(unb) - 1; ++i) {
        const char* nm = BoneName(un[i]);
        o += (size_t)(nm ? _snprintf_s(unb + o, sizeof(unb) - o, _TRUNCATE, "%s%s", o ? "," : "", nm)
                         : _snprintf_s(unb + o, sizeof(unb) - o, _TRUNCATE, "%s%08X", o ? "," : "", un[i]));
    }
    char m[512];
    _snprintf_s(m, sizeof(m), _TRUNCATE,
                "[Pose] matched=%u/%u err p50=%.2fdeg p95=%.2fdeg native p50=%.2fdeg p95=%.2fdeg samples=%d apply=%d puppet=%p unmatched=[%s]\n",
                matched, total, Pct(eo, n, 0.5f), Pct(eo, n, 0.95f), Pct(en, n, 0.5f), Pct(en, n, 0.95f), n, g_apply, ac, unb);
    Phase1_LogLine(m);
    Net::PoseStats ps; Net::GetPoseStats(ps);
    _snprintf_s(m, sizeof(m), _TRUNCATE,
                "[Pose] tick hook cap=%ld app=%ld txSeq=%u tx=%u rx=%u acc=%u rej=%u proto=%u stale=%u capOk=%ld capBusy=%ld applyBusy=%ld appliedTicks=%ld "
                "appliedBones=%ld rootBit0=%ld/%ld clampT=%ld old=%ld ownerMiss=%ld exc=%ld\n",
                c_capHook, c_appHook, s_seq, ps.tx, ps.rx, ps.accepted, ps.rejects, ps.protoMismatch, ps.stale, c_capOk, c_capBusy, c_applyBusy,
                c_appliedTicks, c_appliedBones, c_rootCap, c_rootApp, c_clampT, c_old, c_ownerMiss, c_exc);
    Phase1_LogLine(m);
}
