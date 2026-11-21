// [Coop] CombatTimingProbe (default 0, read once at load). See combat_timing.h for the line catalogue.
// Read-only: it keeps its own tables of pointers used as keys and never writes game memory or the wire. The only game
// memory it reads itself is two entity positions (Entity::m_GlobalMatrix translation) of pointers the caller holds live.
// Lines are buffered and written with the 5 s report (or when the buffer fills), so the probe's file I/O stays off the
// frames it measures.
#include "combat_timing.h"
#include "paths.h"
#include "Scimitar/Entity.h"

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <algorithm>

void Phase1_LogLine(const char* s);

namespace CombatTiming {
namespace {

bool g_on = false;
SRWLOCK g_lock = SRWLOCK_INIT;

// ---- buffered lines -------------------------------------------------------------------------------------------
char g_buf[96 * 1024]; int g_bufN = 0; SRWLOCK g_bufLock = SRWLOCK_INIT; LONG g_lost = 0;
void FlushLocked()
{
    if (!g_bufN) return;
    // Phase1_LogLine formats into 1024 bytes: hand it one line at a time.
    char* p = g_buf; char* end = g_buf + g_bufN;
    while (p < end) {
        char* nl = (char*)memchr(p, '\n', (size_t)(end - p));
        if (!nl) break;
        const char keep = nl[1]; nl[1] = 0;
        Phase1_LogLine(p);
        nl[1] = keep; p = nl + 1;
    }
    g_bufN = 0;
}
void Line(const char* fmt, ...)
{
    char b[420];
    va_list ap; va_start(ap, fmt);
    int n = _vsnprintf_s(b, sizeof(b) - 2, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)strlen(b);
    b[n++] = '\n'; b[n] = 0;
    AcquireSRWLockExclusive(&g_bufLock);
    if (g_bufN + n + 1 >= (int)sizeof(g_buf)) InterlockedIncrement(&g_lost);   // full: dropped, counted in the frame line
    else { memcpy(g_buf + g_bufN, b, (size_t)n + 1); g_bufN += n; }
    ReleaseSRWLockExclusive(&g_bufLock);
}

// ---- bodies ---------------------------------------------------------------------------------------------------
struct Body {
    const void* human; const void* ent; uint64_t id;
    uint32_t alias; float ratio; uint64_t sampleUs, aliasUs;      // the body's own current action (own update)
    uint32_t hostAlias; uint64_t hostAliasUs, seenUs;             // guest: what the host says, and when it arrived
    uint64_t warnUs, hitUs; uint32_t warns;                       // attack series of this guard
};
const int BODY_MAX = 192;
Body g_body[BODY_MAX]; int g_bodyN = 0;
int FindHuman(const void* h) { for (int i = 0; i < g_bodyN; ++i) if (g_body[i].human == h) return i; return -1; }
int FindEnt(const void* e)   { for (int i = 0; i < g_bodyN; ++i) if (g_body[i].ent == e) return i; return -1; }
int FindId(uint64_t id)      { for (int i = 0; i < g_bodyN; ++i) if (g_body[i].id == id) return i; return -1; }

volatile LONG64 g_fightUs = 0;   // last attack event (either side): gates the player-action lines
void NoteFight(uint64_t now) { InterlockedExchange64(&g_fightUs, (LONG64)now); }

// ---- window statistics ----------------------------------------------------------------------------------------
struct Samples {
    uint32_t v[1024]; int n; uint32_t over;
    void Add(uint32_t x) { if (n < 1024) v[n++] = x; else ++over; }
    void Stat(char* out, int cap) {   // "n= p50= p95= p99= max=" in the unit given, then cleared
        if (!n) { _snprintf_s(out, cap, _TRUNCATE, "n=0"); return; }
        std::sort(v, v + n);
        auto P = [&](int pct) { int i = (n * pct) / 100; if (i >= n) i = n - 1; return v[i]; };
        uint64_t sum = 0; for (int i = 0; i < n; ++i) sum += v[i];
        _snprintf_s(out, cap, _TRUNCATE, "n=%d mean=%u p50=%u p95=%u p99=%u max=%u", n + (int)over, (unsigned)(sum / n), P(50), P(95), P(99), v[n - 1]);
        n = 0; over = 0;
    }
};
Samples g_frame, g_snapTx, g_snapApply, g_snapLat;
uint64_t g_lastFrameUs = 0, g_lastSnapTxUs = 0, g_lastApplyUs = 0;
uint32_t g_fgFrames = 0, g_frames = 0, g_over25 = 0, g_over50 = 0, g_reapplies = 0, g_reposts = 0;
uint32_t g_pAlias = 0; int g_pLife = -2;

float Dist(const void* a, const void* b)
{
    if (!a || !b) return -1.0f;
    __try {
        const Entity* x = (const Entity*)a; const Entity* y = (const Entity*)b;
        const float dx = x->m_GlobalMatrix.M41 - y->m_GlobalMatrix.M41, dy = x->m_GlobalMatrix.M42 - y->m_GlobalMatrix.M42,
                    dz = x->m_GlobalMatrix.M43 - y->m_GlobalMatrix.M43;
        return sqrtf(dx * dx + dy * dy + dz * dz);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1.0f; }
}

}  // namespace

uint64_t NowUs()
{
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / f.QuadPart) * 1000000ull + (uint64_t)((c.QuadPart % f.QuadPart) * 1000000ull / f.QuadPart);
}

void Init()
{
    g_on = GetPrivateProfileIntA("Coop", "CombatTimingProbe", 0, Paths::Ini()) != 0;
    if (!g_on) return;
    char line[160];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[CTP] CombatTimingProbe=1 (read once) clock=QPC us t=%llu tick=%lu pid=%lu\n",
                (unsigned long long)NowUs(), (unsigned long)GetTickCount(), (unsigned long)GetCurrentProcessId());
    Phase1_LogLine(line);
}
bool On() { return g_on; }

// ---- both sides -----------------------------------------------------------------------------------------------
void PlayerFrame(bool host, uint32_t playerAlias, float playerRatio, int playerLife)
{
    if (!g_on) return;
    const uint64_t now = NowUs();
    AcquireSRWLockExclusive(&g_lock);
    if (g_lastFrameUs) {
        const uint32_t dt = (uint32_t)(now - g_lastFrameUs);
        if (dt < 2000000) { g_frame.Add(dt); ++g_frames; if (dt > 25000) ++g_over25; if (dt > 50000) ++g_over50; }
    }
    g_lastFrameUs = now;
    HWND fg = GetForegroundWindow(); DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) ++g_fgFrames;
    const uint32_t oldAlias = g_pAlias; const int oldLife = g_pLife;
    g_pAlias = playerAlias; g_pLife = playerLife;
    ReleaseSRWLockExclusive(&g_lock);
    const char r = host ? 'H' : 'G';
    if (oldLife != -2 && playerLife != oldLife) Line("[CTP] %c life t=%llu from=%d to=%d", r, (unsigned long long)now, oldLife, playerLife);
    if (playerAlias != oldAlias && now - (uint64_t)g_fightUs < 8000000)
        Line("[CTP] %c pact t=%llu alias=%u ratio=%.3f", r, (unsigned long long)now, playerAlias, playerRatio);
}

bool Tracked(const void* human)
{
    if (!g_on || !g_bodyN) return false;
    AcquireSRWLockShared(&g_lock);
    const bool t = FindHuman(human) >= 0;
    ReleaseSRWLockShared(&g_lock);
    return t;
}

void BodySample(const void* human, uint32_t alias, float ratio)
{
    if (!g_on) return;
    const uint64_t now = NowUs();
    uint64_t id = 0; uint32_t was = 0, hostAlias = 0; bool changed = false; uint64_t hostAge = 0;
    AcquireSRWLockExclusive(&g_lock);
    const int i = FindHuman(human);
    if (i >= 0) {
        Body& b = g_body[i];
        changed = b.sampleUs != 0 && b.alias != alias;
        was = b.alias; id = b.id; hostAlias = b.hostAlias; hostAge = b.hostAliasUs ? now - b.hostAliasUs : 0;
        if (b.alias != alias || !b.sampleUs) b.aliasUs = now;
        b.alias = alias; b.ratio = ratio; b.sampleUs = now;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!changed) return;
    if (hostAlias)   // guest copy
        Line("[CTP] G body t=%llu id=%016llX alias=%u was=%u ratio=%.3f host=%u hostAgeMs=%llu", (unsigned long long)now,
             (unsigned long long)id, alias, was, ratio, hostAlias, (unsigned long long)(hostAge / 1000));
    else
        Line("[CTP] H anim t=%llu id=%016llX alias=%u was=%u ratio=%.3f", (unsigned long long)now, (unsigned long long)id, alias, was, ratio);
}

void ScopeBody(const void* human, const void* ent, uint64_t objId)
{
    if (!g_on || !human || !ent) return;
    const uint64_t now = NowUs();
    AcquireSRWLockExclusive(&g_lock);
    int i = FindHuman(human);
    if (i < 0) {
        if (g_bodyN >= BODY_MAX) {   // evict the entry not seen for the longest time
            int old = 0; for (int k = 1; k < g_bodyN; ++k) if (g_body[k].seenUs < g_body[old].seenUs) old = k;
            i = old;
        } else i = g_bodyN++;
        memset(&g_body[i], 0, sizeof(Body));
        g_body[i].human = human;
    }
    g_body[i].ent = ent; g_body[i].id = objId; g_body[i].seenUs = now;
    ReleaseSRWLockExclusive(&g_lock);
}

void Report(bool host)
{
    if (!g_on) return;
    char f[160], a[160], b[160], c[160];
    const uint64_t now = NowUs();
    AcquireSRWLockExclusive(&g_lock);
    g_frame.Stat(f, sizeof(f)); g_snapTx.Stat(a, sizeof(a)); g_snapApply.Stat(b, sizeof(b)); g_snapLat.Stat(c, sizeof(c));
    const uint32_t frames = g_frames, fg = g_fgFrames, o25 = g_over25, o50 = g_over50, re = g_reapplies, rp = g_reposts;
    g_frames = g_fgFrames = g_over25 = g_over50 = g_reapplies = g_reposts = 0;
    for (int i = 0; i < g_bodyN;) {   // bodies no snapshot named for 5 s
        if (now - g_body[i].seenUs > 5000000) g_body[i] = g_body[--g_bodyN]; else ++i;
    }
    const int bodies = g_bodyN;
    ReleaseSRWLockExclusive(&g_lock);
    DEVMODEA dm = {}; dm.dmSize = sizeof(dm);
    const int hz = EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm) ? (int)dm.dmDisplayFrequency : -1;
    const char r = host ? 'H' : 'G';
    Line("[CTP] %c frame t=%llu us %s | >25ms=%u >50ms=%u | foreground=%u/%u | displayHz=%d remote=%d | bodies=%d lostLines=%ld",
         r, (unsigned long long)now, f, o25, o50, fg, frames, hz, GetSystemMetrics(SM_REMOTESESSION), bodies, g_lost);
    if (host) Line("[CTP] H lane t=%llu snapshot send interval us %s", (unsigned long long)now, a);
    else Line("[CTP] G lane t=%llu snapshot apply interval us %s | capture->apply ms %s | reapplies=%u warnReposts=%u",
              (unsigned long long)now, b, c, re, rp);
    AcquireSRWLockExclusive(&g_bufLock);
    FlushLocked();
    ReleaseSRWLockExclusive(&g_bufLock);
}

// ---- host -----------------------------------------------------------------------------------------------------
void HostFde(uintptr_t attackerEnt, uintptr_t puppetEnt, int phase, uint32_t type, uint32_t sub)
{
    if (!g_on || !attackerEnt) return;
    const uint64_t now = NowUs();
    NoteFight(now);
    uint64_t id = 0; uint32_t alias = 0, warns = 0; float ratio = 0; uint64_t age = 0, aliasAge = 0, lastWarnAge = 0, firstWarnAge = 0;
    bool log = false;
    static uint64_t s_firstWarn[BODY_MAX];
    AcquireSRWLockExclusive(&g_lock);
    const int i = FindEnt((const void*)attackerEnt);
    if (i >= 0) {
        Body& b = g_body[i];
        id = b.id; alias = b.alias; ratio = b.ratio; age = b.sampleUs ? now - b.sampleUs : 0; aliasAge = b.aliasUs ? now - b.aliasUs : 0;
        if (!phase) {
            if (now - b.warnUs > 300000) { log = true; b.warns = 0; s_firstWarn[i] = now; }
            b.warnUs = now; ++b.warns;
        } else if (now - b.hitUs > 150000) {   // each hit is posted twice ~16 ms apart
            log = true; b.hitUs = now; warns = b.warns; lastWarnAge = b.warnUs ? now - b.warnUs : 0;
            firstWarnAge = s_firstWarn[i] ? now - s_firstWarn[i] : 0; b.warns = 0;
        }
    } else log = phase != 0;   // not in scope: hits only, by entity
    ReleaseSRWLockExclusive(&g_lock);
    if (!log) return;
    const float d = Dist((const void*)attackerEnt, (const void*)puppetEnt);
    if (!phase)
        Line("[CTP] H fde t=%llu id=%016llX ent=%08X ph=0 type=%u sub=%u | alias=%u ratio=%.3f sampleAgeMs=%llu aliasAgeMs=%llu | d=%.2f",
             (unsigned long long)now, (unsigned long long)id, (unsigned)attackerEnt, type, sub, alias, ratio,
             (unsigned long long)(age / 1000), (unsigned long long)(aliasAge / 1000), d);
    else
        Line("[CTP] H fde t=%llu id=%016llX ent=%08X ph=1 type=%u sub=%u | alias=%u ratio=%.3f sampleAgeMs=%llu aliasAgeMs=%llu | d=%.2f | warns=%u firstWarnAgeMs=%llu lastWarnAgeMs=%llu",
             (unsigned long long)now, (unsigned long long)id, (unsigned)attackerEnt, type, sub, alias, ratio,
             (unsigned long long)(age / 1000), (unsigned long long)(aliasAge / 1000), d, warns,
             (unsigned long long)(firstWarnAge / 1000), (unsigned long long)(lastWarnAge / 1000));
}

void HostAtkTx(uint64_t guardId, uint32_t seq, int phase, bool ok)
{
    if (!g_on) return;
    Line("[CTP] H tx t=%llu id=%016llX seq=%u ph=%d ok=%d", (unsigned long long)NowUs(), (unsigned long long)guardId, seq, phase, ok ? 1 : 0);
}

void HostPdrTx(uint64_t guardId, uint32_t count, int dmg)
{
    if (!g_on) return;
    const uint64_t now = NowUs(); NoteFight(now);
    Line("[CTP] H pdr t=%llu id=%016llX n=%u dmg=%d", (unsigned long long)now, (unsigned long long)guardId, count, dmg);
}

void HostSnapshotSent(uint32_t hostMs, uint32_t records)
{
    if (!g_on) return;
    (void)hostMs; (void)records;
    const uint64_t now = NowUs();
    AcquireSRWLockExclusive(&g_lock);
    if (g_lastSnapTxUs && now - g_lastSnapTxUs < 5000000) g_snapTx.Add((uint32_t)(now - g_lastSnapTxUs));
    g_lastSnapTxUs = now;
    ReleaseSRWLockExclusive(&g_lock);
}

// ---- guest ----------------------------------------------------------------------------------------------------
void GuestSnapshotApplied(uint32_t seq, uint32_t hostMs, bool newSeq)
{
    if (!g_on) return;
    (void)seq;
    const uint64_t now = NowUs();
    AcquireSRWLockExclusive(&g_lock);
    if (!newSeq) ++g_reapplies;
    else {
        if (g_lastApplyUs && now - g_lastApplyUs < 5000000) g_snapApply.Add((uint32_t)(now - g_lastApplyUs));
        g_lastApplyUs = now;
        // Same PC: the host stamps QPC ms truncated to 32 bits, and this copy reads the same counter.
        if (hostMs) { const int32_t lat = (int32_t)((uint32_t)(now / 1000) - hostMs); if (lat >= 0 && lat < 60000) g_snapLat.Add((uint32_t)lat); }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void GuestGuardRx(uint64_t objId, const void* ent, uint32_t hostAlias, uint16_t ratioQ, uint32_t hostMs)
{
    if (!g_on) return;
    const uint64_t now = NowUs();
    bool changed = false; uint32_t body = 0; float bodyRatio = 0;
    AcquireSRWLockExclusive(&g_lock);
    const int i = FindEnt(ent);
    if (i >= 0) {
        Body& b = g_body[i];
        b.id = objId; b.seenUs = now;
        if (b.hostAlias != hostAlias) { changed = b.hostAlias != 0; b.hostAlias = hostAlias; b.hostAliasUs = now; }
        body = b.alias; bodyRatio = b.ratio;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (changed)
        Line("[CTP] G rx t=%llu id=%016llX alias=%u hostRatio=%.3f hostMs=%u latMs=%d | body alias=%u ratio=%.3f",
             (unsigned long long)now, (unsigned long long)objId, hostAlias, ratioQ / 65535.0f, hostMs,
             hostMs ? (int)(int32_t)((uint32_t)(now / 1000) - hostMs) : -1, body, bodyRatio);
}

void GuestMirrorAct(const void* ent, uint64_t objId, uint32_t alias)
{
    if (!g_on) return;
    (void)ent;
    Line("[CTP] G act t=%llu id=%016llX alias=%u", (unsigned long long)NowUs(), (unsigned long long)objId, alias);
}

static void GuestEvent(const char* what, uint64_t guardId, uint32_t n, int phase, int extra, const void* copyEnt, const void* playerEnt,
                       bool posted, int lifeBefore)
{
    const uint64_t now = NowUs();
    NoteFight(now);
    uint32_t alias = 0, hostAlias = 0, warns = 0; float ratio = 0; uint64_t age = 0, aliasAge = 0, hostAge = 0, firstWarnAge = 0, lastWarnAge = 0;
    bool log = true;
    static uint64_t s_firstWarn[BODY_MAX];
    AcquireSRWLockExclusive(&g_lock);
    int i = copyEnt ? FindEnt(copyEnt) : -1;
    if (i < 0) i = FindId(guardId);
    if (i >= 0) {
        Body& b = g_body[i];
        alias = b.alias; ratio = b.ratio; age = b.sampleUs ? now - b.sampleUs : 0; aliasAge = b.aliasUs ? now - b.aliasUs : 0;
        hostAlias = b.hostAlias; hostAge = b.hostAliasUs ? now - b.hostAliasUs : 0;
        if (!phase) {
            if (now - b.warnUs > 300000) { b.warns = 0; s_firstWarn[i] = now; } else log = false;
            b.warnUs = now; ++b.warns;
        } else {
            warns = b.warns; lastWarnAge = b.warnUs ? now - b.warnUs : 0; firstWarnAge = s_firstWarn[i] ? now - s_firstWarn[i] : 0;
            b.warns = 0; b.hitUs = now;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!log) return;
    const float d = Dist(copyEnt, playerEnt);
    Line("[CTP] G %s t=%llu id=%016llX n=%u ph=%d x=%d copy=%08X posted=%d | body alias=%u ratio=%.3f sampleAgeMs=%llu aliasAgeMs=%llu host=%u hostAgeMs=%llu | d=%.2f life=%d | warns=%u firstWarnAgeMs=%llu lastWarnAgeMs=%llu",
         what, (unsigned long long)now, (unsigned long long)guardId, n, phase, extra, (unsigned)(uintptr_t)copyEnt, posted ? 1 : 0,
         alias, ratio, (unsigned long long)(age / 1000), (unsigned long long)(aliasAge / 1000), hostAlias,
         (unsigned long long)(hostAge / 1000), d, lifeBefore, warns, (unsigned long long)(firstWarnAge / 1000),
         (unsigned long long)(lastWarnAge / 1000));
}

void GuestAtk(uint64_t guardId, uint32_t seq, int phase, uint32_t msgHostMs, const void* copyEnt, const void* playerEnt,
              bool posted, int lifeBefore)
{
    if (!g_on) return;
    // x = lane age in ms on the GetTickCount clock the publisher stamped (same PC only; 15.6 ms steps)
    GuestEvent("atk", guardId, seq, phase, (int)(int32_t)(GetTickCount() - msgHostMs), copyEnt, playerEnt, posted, lifeBefore);
}

void GuestAtkRepost()
{
    if (!g_on) return;
    AcquireSRWLockExclusive(&g_lock); ++g_reposts; ReleaseSRWLockExclusive(&g_lock);
}

void GuestPdr(uint64_t guardId, uint32_t count, int dmg, const void* copyEnt, const void* playerEnt, bool posted, int lifeBefore)
{
    if (!g_on) return;
    GuestEvent("pdr", guardId, count, 1, dmg, copyEnt, playerEnt, posted, lifeBefore);
}

}  // namespace CombatTiming
