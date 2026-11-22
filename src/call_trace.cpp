#include "call_trace.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <process.h>
#include "paths.h"

void Phase1_LogLine(const char* s);   // phase1.cpp

namespace {
struct Slot { uint32_t vt, idx, target; volatile LONG calls; };
constexpr int kMax = 512;
Slot g_slot[kMax];
int g_n = 0;
uint8_t* g_stubs = nullptr;

void Log(const char* fmt, ...) {
    char b[400];
    va_list ap; va_start(ap, fmt); _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, ap); va_end(ap);
    Phase1_LogLine(b);
}

void __cdecl CtLog(uint32_t id, uint32_t ecx, uint32_t ret, uint32_t arg1) {
    if (id >= (uint32_t)g_n) return;
    const LONG c = InterlockedIncrement(&g_slot[id].calls);
    if (c <= 12)
        Log("[CT] vt=%08X slot=%u fn=%08X this=%08X ret=%08X arg1=%08X tid=%u n=%ld\n", g_slot[id].vt, g_slot[id].idx,
            g_slot[id].target, ecx, ret, arg1, GetCurrentThreadId(), c);
}

unsigned __stdcall Reporter(void*) {
    LONG last[kMax] = {};
    for (;;) {
        Sleep(10000);
        for (int i = 0; i < g_n; ++i) {
            const LONG c = g_slot[i].calls;
            if (c != last[i] && c > 12)
                Log("[CT] sum vt=%08X slot=%u fn=%08X calls=%ld (+%ld)\n", g_slot[i].vt, g_slot[i].idx, g_slot[i].target, c, c - last[i]);
            last[i] = c;
        }
    }
}

bool InCode(uint32_t a) { return a >= 0x00401000u && a < 0x02400000u; }

void Emit(uint8_t* p, uint32_t id, uint32_t orig) {
    int k = 0;
    auto b = [&](uint8_t v) { p[k++] = v; };
    auto d = [&](uint32_t v) { memcpy(p + k, &v, 4); k += 4; };
    b(0x60); b(0x9C);                               // pushad; pushfd
    b(0xFF); b(0x74); b(0x24); b(0x28);             // push [esp+28h]  first stack arg (original [esp+4])
    b(0xFF); b(0x74); b(0x24); b(0x28);             // push [esp+28h]  return address (original [esp])
    b(0x51);                                        // push ecx
    b(0x68); d(id);                                 // push id
    b(0xB8); d((uint32_t)(uintptr_t)&CtLog);        // mov eax, CtLog
    b(0xFF); b(0xD0);                               // call eax
    b(0x83); b(0xC4); b(0x10);                      // add esp, 16
    b(0x9D); b(0x61);                               // popfd; popad
    b(0x68); d(orig);                               // push orig
    b(0xC3);                                        // ret
}
}  // namespace

namespace CallTrace {
void Init() {
    char spec[1024] = {};
    GetPrivateProfileStringA("Coop", "CallTraceVt", "", spec, sizeof(spec), Paths::Ini());
    if (!spec[0]) return;
    struct Vt { uint32_t va, n; } vts[32]; int nv = 0;
    for (char* tok = strtok(spec, ", "); tok && nv < 32; tok = strtok(nullptr, ", ")) {
        char* colon = strchr(tok, ':');
        if (!colon) continue;
        *colon = 0;
        vts[nv].va = strtoul(tok, nullptr, 16); vts[nv].n = strtoul(colon + 1, nullptr, 10);
        if (vts[nv].n > 96) vts[nv].n = 96;
        ++nv;
    }
    // A target that appears in more than one listed vtable is a shared base method: skipped.
    auto shared = [&](uint32_t t, int self) {
        for (int v = 0; v < nv; ++v) {
            if (v == self) continue;
            for (uint32_t s = 0; s < vts[v].n; ++s) if (((uint32_t*)(uintptr_t)vts[v].va)[s] == t) return true;
        }
        return false;
    };
    g_stubs = (uint8_t*)VirtualAlloc(nullptr, kMax * 40, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_stubs) { Log("[CT] VirtualAlloc failed\n"); return; }
    int skipped = 0;
    for (int v = 0; v < nv; ++v) {
        uint32_t* tab = (uint32_t*)(uintptr_t)vts[v].va;
        for (uint32_t s = 0; s < vts[v].n && g_n < kMax; ++s) {
            const uint32_t t = tab[s];
            if (!InCode(t) || shared(t, v)) { ++skipped; continue; }
            const int id = g_n++;
            g_slot[id] = { vts[v].va, s, t, 0 };
            uint8_t* stub = g_stubs + id * 40;
            Emit(stub, (uint32_t)id, t);
            DWORD old = 0;
            if (VirtualProtect(&tab[s], 4, PAGE_READWRITE, &old)) {
                InterlockedExchange((volatile LONG*)&tab[s], (LONG)(uintptr_t)stub);
                VirtualProtect(&tab[s], 4, old, &old);
            }
        }
    }
    FlushInstructionCache(GetCurrentProcess(), g_stubs, kMax * 40);
    Log("[CT] installed %d slot stubs over %d vtables (skipped %d shared or non-code)\n", g_n, nv, skipped);
    CloseHandle((HANDLE)_beginthreadex(nullptr, 0, Reporter, nullptr, 0, nullptr));
}
}  // namespace CallTrace
