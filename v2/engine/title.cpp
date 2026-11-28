#include "title.h"
#include "../core/log.h"
#include "../core/mem.h"

#include <windows.h>

namespace v2::engine::title {
namespace {

volatile LONG g_startCalls = 0, g_lastStartTick = 0, g_continueDone = 0;

// Slot record i (handle idiom of sub_132A6D0): 1 = has a save, 0 = empty, -1 = unreadable / index mismatch.
int SlotHasSave(int i) {
    uint32_t mgr = 0, arr = 0, h = 0, p = 0, f = 0, idx = 0; uint8_t b[3] = {};
    if (!mem::Read(0x04E38128u, &mgr) || !mem::Read(mgr + 4484, &arr) || !mem::Read(arr + 4 * (uint32_t)i, &h) ||
        !mem::Read(h, &p) || !mem::Read(h + 8, &f)) return -1;
    const uint32_t obj = (uint32_t)(((int32_t)f >> 31) & (int32_t)p);
    if (!obj || !mem::Read(obj + 36, &idx) || idx != (uint32_t)i || !mem::Read((const void*)(uintptr_t)(obj + 64), b, 3)) return -1;
    return (b[0] | b[1] | b[2]) ? 1 : 0;   // bytes +64..+66 all zero = sub_18DAC60's NEW_GAME branch
}

}  // namespace

void TryPressStart(void* self) {
    uint32_t state = 0, ent = 0, d8 = 0;
    if (g_startCalls >= 3 || !mem::Read((uintptr_t)self + 8, &state) || (state != 2 && state != 3)) return;
    if (!mem::Read((uintptr_t)self + 0x28, &ent) || !ent || !mem::Read(ent + 0xD8, &d8) || d8 != 2) return;
    const LONG now = (LONG)GetTickCount(), last = g_lastStartTick;
    if (last && now - last < 700) return;
    if (InterlockedCompareExchange(&g_lastStartTick, now, last) != last) return;   // another worker took this tick
    if (!((unsigned char(__thiscall*)(void*))0x00F91B20)((void*)(uintptr_t)ent)) return;   // sub_F91B20: type check
    LOGI("[Boot] press start (attempt %ld)\n", InterlockedIncrement(&g_startCalls));
    ((int(__thiscall*)(void*))0x014ACD10)((void*)(uintptr_t)ent);                          // sub_14ACD10
}

void TryContinue(void* menu, uint32_t highlighted, int wantSlot) {
    uint32_t g = 0; uint8_t ready = 0;
    if (!mem::Read(0x02ABE524u, &g) || !mem::Read((const void*)(uintptr_t)(g + 138), &ready, 1) || !ready) return;
    if (InterlockedCompareExchange(&g_continueDone, 1, 0) != 0) return;                       // once per process
    const int s[3] = { SlotHasSave(0), SlotHasSave(1), SlotHasSave(2) };
    int slot = -1;
    if (wantSlot >= 0 && wantSlot <= 2) { if (s[wantSlot] == 1) slot = wantSlot; }
    else if (highlighted <= 2 && s[highlighted] == 1) slot = (int)highlighted;
    else for (int i = 0; i < 3 && slot < 0; ++i) if (s[i] == 1) slot = i;
    LOGI("[Boot] continue: records %d %d %d -> slot %d\n", s[0], s[1], s[2], slot);
    if (slot >= 0) ((int(__thiscall*)(void*, uint32_t))0x018DAC60)(menu, (uint32_t)slot);   // sub_18DAC60
}

}  // namespace v2::engine::title
