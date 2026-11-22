#include "ground_tick_probe.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include "paths.h"

void Phase1_LogLine(const char* s);   // phase1.cpp

namespace {
constexpr uintptr_t kSlot = 0x026E8B24;   // Ground context vtable 0x026E8AE8, slot 15 (0x3C)
constexpr uintptr_t kOrig = 0x01D80950;   // sub_1D80950, the only thing that slot may hold

typedef int(__thiscall* UpdateFn)(void*, int);
UpdateFn g_orig = nullptr;
int g_on = 0;

// Open-addressed ctx -> call count. Written from whatever thread runs the update; interlocked only.
constexpr int kCap = 1024;
void* volatile g_key[kCap];
volatile LONG g_cnt[kCap];
volatile LONG g_full = 0;

int Slot(void* ctx, bool insert) {
    uintptr_t h = ((uintptr_t)ctx >> 4) * 2654435761u;
    for (int i = 0; i < kCap; ++i) {
        int s = (int)((h + i) & (kCap - 1));
        void* k = g_key[s];
        if (k == ctx) return s;
        if (!k) {
            if (!insert) return -1;
            void* prev = InterlockedCompareExchangePointer((PVOID volatile*)&g_key[s], ctx, nullptr);
            if (!prev || prev == ctx) return s;
        }
    }
    if (insert) InterlockedIncrement(&g_full);
    return -1;
}

int __fastcall Thunk(void* ctx, void* /*edx*/, int a2) {
    int s = Slot(ctx, true);
    if (s >= 0) InterlockedIncrement(&g_cnt[s]);
    return g_orig(ctx, a2);
}

bool Readable(const void* p, size_t n) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

void Log(const char* fmt, ...) {
    char b[512];
    va_list ap; va_start(ap, fmt); _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, ap); va_end(ap);
    Phase1_LogLine(b);
}

struct Seen { void* ent; void* ctx; int role; LONG last; uint64_t ms; };
constexpr int kSeen = 64;
Seen g_seen[kSeen];
int g_seenN = 0;
uint64_t g_nextReport = 0;
const char* kRole[5] = { "?", "copy", "native", "puppet", "player" };

void Report(uint64_t now) {
    // Per role: how many noted bodies advanced since the last report, then up to 3 detail lines per role.
    for (int role = 1; role <= 4; ++role) {
        int n = 0, ticked = 0, shown = 0;
        for (int i = 0; i < g_seenN; ++i) {
            Seen& s = g_seen[i];
            if (s.role != role || now - s.ms > 4000) continue;   // only bodies noted recently
            int k = Slot(s.ctx, false);
            LONG c = (k >= 0) ? g_cnt[k] : 0;
            LONG d = c - s.last;
            s.last = c;
            ++n; if (d > 0) ++ticked;
            if (shown < 3) {
                ++shown;
                unsigned cnt = 0xFFFF, st0 = 0xFFFF;
                __try {
                    char* helper = Readable((char*)s.ctx + 0xA0, 4) ? *(char**)((char*)s.ctx + 0xA0) : nullptr;
                    if (helper && Readable(helper + 0x118, 8)) {
                        cnt = *(unsigned short*)(helper + 0x11E);
                        unsigned short* ids = *(unsigned short**)(helper + 0x118);
                        if (cnt && Readable(ids, 2)) st0 = ids[0];
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
                Log("[GTick] %s e=%p ctx=%p calls=%ld (+%ld) states=%u s0=%u\n", kRole[role], s.ent, s.ctx, c, d, cnt, st0);
            }
        }
        if (n) Log("[GTick] sum role=%s bodies=%d ticked=%d\n", kRole[role], n, ticked);
    }
    if (g_full) Log("[GTick] table full drops=%ld\n", g_full);
}
}  // namespace

namespace GroundTick {
bool On() { return g_on != 0; }

void Init() {
    g_on = GetPrivateProfileIntA("Coop", "GroundTickProbe", 0, Paths::Ini()) != 0 ? 1 : 0;
    if (!g_on) return;
    uintptr_t cur = *(uintptr_t*)kSlot;
    if (cur != kOrig) { g_on = 0; Log("[GTick] install REFUSED: slot %08X holds %08X, expected %08X\n", (unsigned)kSlot, (unsigned)cur, (unsigned)kOrig); return; }
    g_orig = (UpdateFn)cur;
    DWORD old = 0;
    if (!VirtualProtect((void*)kSlot, 4, PAGE_READWRITE, &old)) { g_on = 0; Log("[GTick] install FAILED: VirtualProtect\n"); return; }
    InterlockedExchange((volatile LONG*)kSlot, (LONG)(uintptr_t)&Thunk);
    VirtualProtect((void*)kSlot, 4, old, &old);
    Log("[GTick] installed slot %08X -> counter (orig %08X)\n", (unsigned)kSlot, (unsigned)kOrig);
}

void Note(void* entity, void* ctx, int role) {
    if (!g_on || !ctx || role < 1 || role > 4) return;
    const uint64_t now = GetTickCount64();
    int i = 0;
    for (; i < g_seenN; ++i) if (g_seen[i].ctx == ctx) break;
    if (i == g_seenN) {
        if (g_seenN >= kSeen) return;
        int k = Slot(ctx, false);
        g_seen[g_seenN++] = { entity, ctx, role, (k >= 0) ? g_cnt[k] : 0, now };
    } else {
        g_seen[i].ms = now; g_seen[i].role = role; g_seen[i].ent = entity;
    }
    if (now >= g_nextReport) { g_nextReport = now + 2000; Report(now); }
}
}  // namespace GroundTick
