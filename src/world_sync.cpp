// World-state sync: time of day, collectables (probe), coordinator trigger relay. See world_sync.h and
// docs/design/world-state-sync.md. Evidence for every address below:
// ops/evidence/world-state-tod-collectables-coord-2026-11-27.md (static RE, IDA session 4b0f0b4a).
//
// Thread rules. Everything except the detour handler runs on the player-update (game) thread: Tick() from
// OnFrameInner next to CoordSyncTick(), OnCoordMsg() from FightIntentDrain (the intent lane's single consumer).
// The only engine function this file calls is the ToD setter 0x0040D0C0, which takes no lock (evidence Q1.2:
// 0x40D0C0 and 0x45ED50 read in full, no lock/interlocked instruction), so it may run under __try.
#include "world_sync.h"
#include "coord_sync.h"
#include "coord_enum.h"
#include "paths.h"
#include "net.h"
#include "netstate.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>

bool Phase1_IsEntityHost();
void Phase1_LogLine(const char* s);

namespace {

bool Readable(const void* p, size_t n) {
    const uintptr_t a = (uintptr_t)p;
    if (!a || a < 0x10000) return false;
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery(p, &m, sizeof(m)) || m.State != MEM_COMMIT) return false;
    if (m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    if (!(m.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                       PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    const uintptr_t end = a + n;
    return end >= a && end <= (uintptr_t)m.BaseAddress + m.RegionSize;
}

void Log(const char* fmt, ...) {
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    Phase1_LogLine(line);
}

// ---- flags: read once (the s_x = -1 idiom) -------------------------------------------------------------
int TodOn() { static int s = -1; if (s < 0) s = GetPrivateProfileIntA("Coop", "TimeOfDaySync", 0, Paths::Ini()) != 0 ? 1 : 0; return s; }

// =========================================================================================================
// [Coop] TimeOfDaySync (evidence Q1.2 / Q1.3)
// =========================================================================================================
// World    = *(*(u32*)0x02AC1630 + 0x50)        sub_54D4D0: mov eax,[2AC1630h]; test; mov eax,[eax+50h]
// ToD mgr  = *(World + 0x8F0)                   cached by sub_71C160 (TimeOfDayManager, vtable 0x01E3F878)
// hours    = *(float*)*(u32*)(ToD + 0xB4)        sub_409AC0: mov eax,[ecx+0B4h]; fld dword ptr [eax]
// setter   = vtable slot +0x6C = 0x0040D0C0      __thiscall(ToD*, float), retn 4; fmod 24 then sub_45ED50 store
// Wire: IC_COORD, wireId = kTodKey (NOT kCoordKeyLocal: the lane keeps ONE persisted value per (wireId, channel),
// and key 0 already carries the local ship's hull/sail - a 1 Hz ToD value there would replace that replay base).
const uint32_t kWorldGlobal = 0x02AC1630u;
const uint32_t kTodVt       = 0x01E3F878u;
const uint32_t kTodSetter   = 0x0040D0C0u;
const uint64_t kTodKey      = 0xFFFFFFFF544F4448ull;   // 'TODH', high dword 0xFFFFFFFF: not an entity id [UNVERIFIED
                                                      // that no entity id has this value; minted ids are 0xF0..0xF7]
const float    kTodEps      = 0.05f;                  // hours
const DWORD    kTodPubMs    = 1000;
const DWORD    kTodReportMs = 10000;

// The ToD manager, every hop Readable() and the vtable checked; nullptr when any hop fails.
void* TodManager() {
    if (!Readable((const void*)(uintptr_t)kWorldGlobal, 4)) return nullptr;
    const uint8_t* g = *(const uint8_t* const*)(uintptr_t)kWorldGlobal;
    if (!Readable(g + 0x50, 4)) return nullptr;
    const uint8_t* w = *(const uint8_t* const*)(g + 0x50);
    if (!Readable(w + 0x8F0, 4)) return nullptr;
    uint8_t* tod = *(uint8_t* const*)(w + 0x8F0);
    if (!Readable(tod, 0xB8)) return nullptr;
    if (*(const uint32_t*)tod != kTodVt) return nullptr;
    return tod;
}

bool TodRead(float* out) {
    __try {
        const uint8_t* tod = (const uint8_t*)TodManager();
        if (!tod) return false;
        const float* clk = *(const float* const*)(tod + 0xB4);
        if (!Readable(clk, 4)) return false;
        const float h = *clk;
        if (!(h >= 0.0f && h < 24.0f)) return false;        // also rejects NaN
        *out = h;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// a - b on the 24 h circle, in (-12, 12]. a, b in [0, 24) so the fmod argument is in (12, 60).
float TodDiff(float a, float b) { return fmodf(a - b + 36.0f, 24.0f) - 12.0f; }

// Guest apply. Gates (MP sub_498A91 template): vtable == TimeOfDayManager, slot +0x6C == 0x0040D0C0, 0 <= h < 24.
// The setter takes no lock (evidence Q1.2), so it runs inside __try: a fault costs this apply, not the frame.
int TodApply(float h) {
    if (!(h >= 0.0f && h < 24.0f)) return 0;
    __try {
        void* tod = TodManager();
        if (!tod) return 0;
        const uint32_t* vtbl = *(const uint32_t* const*)tod;
        if (!Readable(vtbl + 0x6C / 4, 4) || vtbl[0x6C / 4] != kTodSetter) return 0;
        // Unit: decomp/units.json 0040D0C0 "TimeOfDayManager_SetCurrentTime".
        ((void (__thiscall*)(void*, float))0x0040D0C0)(tod, h);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

LONG  s_todTx = 0, s_todRx = 0, s_todApplied = 0, s_todRefused = 0, s_todExc = 0, s_todBad = 0;
float s_todHost = -1.0f;      // guest: last host value received
DWORD s_todHostMs = 0;        // guest: when it arrived
uint32_t s_todSeq = 0;
bool  s_todPending = false;   // guest: a new host value that has not been compared yet

void TodHostTick(DWORD now) {
    static DWORD s_lastPub = 0;
    static float s_lastVal = -1.0f;
    float h = 0.0f;
    if (!TodRead(&h)) return;
    const bool due = !s_lastPub || now - s_lastPub >= kTodPubMs ||
                     (s_lastVal >= 0.0f && fabsf(TodDiff(h, s_lastVal)) > kTodEps);
    if (!due) return;
    const IntentCoordValue p = { (uint8_t)CF_TOD_HOURS, { 0, 0, 0 }, h };
    if (Net::PublishIntent(0, kTodKey, IC_COORD, (uint32_t)now, &p, (uint8_t)sizeof(p))) ++s_todTx;
    s_lastPub = now; s_lastVal = h;
}

void TodGuestTick(DWORD now) {
    if (!s_todPending) return;
    s_todPending = false;                       // one compare per received value: the guest clock keeps running
    float before = -1.0f;                       // between messages, so re-applying a 1 s old value every frame
    if (!TodRead(&before)) return;              // would drag it backwards
    const float host = s_todHost;
    const float d = TodDiff(host, before);
    if (fabsf(d) <= kTodEps) return;
    const int ok = TodApply(host);
    if (ok > 0) ++s_todApplied; else if (ok < 0) ++s_todExc; else ++s_todRefused;
    float after = -1.0f;
    TodRead(&after);
    Log("[ToD] apply host=%.3f before=%.3f after=%.3f ok=%d age=%lums\n", host, before, after, ok,
        (unsigned long)(now - s_todHostMs));
}

void TodReport(DWORD now, bool host) {
    static DWORD s_next = 0;
    if (s_next && now - s_next < kTodReportMs) return;
    s_next = now;
    float local = -1.0f;
    const bool have = TodRead(&local);
    if (host) {
        Log("[ToD] report role=host host=%.3f guest=n/a applied=0 tx=%ld\n", have ? local : -1.0f, s_todTx);
    } else {
        const float d = (have && s_todHost >= 0.0f) ? TodDiff(s_todHost, local) : 0.0f;
        Log("[ToD] report role=guest host=%.3f guest=%.3f d=%.3f applied=%ld rx=%ld refused=%ld exc=%ld bad=%ld age=%lums\n",
            s_todHost, have ? local : -1.0f, d, s_todApplied, s_todRx, s_todRefused, s_todExc, s_todBad,
            s_todHostMs ? (unsigned long)(now - s_todHostMs) : 0ul);
    }
}

void TodRx(uint32_t seq, uint8_t flags, float v) {
    if (!TodOn()) { ++s_todBad; return; }
    if (Phase1_IsEntityHost()) return;                       // the host is the authority; never adopt a value
    if (!(v >= 0.0f && v < 24.0f)) { ++s_todBad; return; }
    if (!(flags & IF_BASE) && s_todSeq && seq <= s_todSeq) { ++s_todBad; return; }
    s_todSeq = seq; s_todHost = v; s_todHostMs = GetTickCount(); s_todPending = true;
    ++s_todRx;
}

} // namespace

void WorldSync::Init() {
    TodOn();
}

bool WorldSync::DrainNeeded() {
    return TodOn() && Net::IntentLaneOn();
}

void WorldSync::Tick() {
    const DWORD now = GetTickCount();
    if (TodOn() && Net::IntentLaneOn()) {
        const bool host = Phase1_IsEntityHost();
        if (host) TodHostTick(now); else TodGuestTick(now);
        TodReport(now, host);
    }
}

bool WorldSync::OnCoordMsg(uint64_t wireId, uint32_t seq, uint8_t flags, const uint8_t* payload, uint8_t len) {
    if (!payload || len != sizeof(IntentCoordValue)) return false;
    IntentCoordValue p;
    memcpy(&p, payload, sizeof(p));
    if (p.field == CF_TOD_HOURS) {
        if (wireId == kTodKey) TodRx(seq, flags, p.value); else ++s_todBad;
        return true;
    }
    return false;
}
