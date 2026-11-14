// Pilot: drive Edward from a client process and publish his surroundings. See pilot.h for the contract.
//
// Engine knowledge used here (all already used elsewhere in the plugin, no new engine call):
//   World::GetCurrentWorld (0x0054D4D0) / World::GetPlayer (0x0070E810)   Scimitar/World.h, decomp units exist
//   World::m_CharacterList, BaseEntity::m_GlobalMatrix                    layout from the compiler (Scimitar/*.h)
//   entity +0xE8 = AI component, AIC +0x98 = fighter record, rec +0x18 role, rec +0x2C status
//                                                                         ops/evidence/guard-ai/fightmanager.md (IDA)
//   entity +0x9C kind/role word                                           sub_73B4B0 / sub_54EC00 (PS3-P1-RESULTS.md)
// Imports patched (IAT only; AC4BFSP.exe imports xinput1_3 by ordinal: #2 GetState, #3 SetState, #4 GetCapabilities,
// and user32 GetForegroundWindow / GetActiveWindow - read from the PE import table of md5 2058342866688f780c8b34526a65bc35).
#include "pilot.h"
#include "paths.h"
#include "Hook.h"
#include "Scimitar/Entity.h"
#include "Scimitar/World.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <cstddef>

void Phase1_LogLine(const char* s);

// tools/pilot.py mirrors these layouts (PAD_FMT / CHAR_FMT / HDR_FMT); the compiler checks them here (LESSONS.md §1).
static_assert(sizeof(Pilot::PilotPad) == 40, "PilotPad layout changed: update tools/pilot.py");
static_assert(sizeof(Pilot::PilotChar) == 40, "PilotChar layout changed: update tools/pilot.py");
static_assert(offsetof(Pilot::PilotState, seq) == 8 && offsetof(Pilot::PilotState, m) == 32 &&
              offsetof(Pilot::PilotState, chars) == 120 && sizeof(Pilot::PilotState) == 10360,
              "PilotState layout changed: update tools/pilot.py");

namespace Pilot {
namespace {

int g_enabled = 0, g_keepFocus = 0, g_padIndex = 0;
volatile LONG g_iatDone = 0, g_focusDone = 0;
PilotPad*   g_pad = nullptr;
PilotState* g_state = nullptr;
HWND        g_wnd = nullptr;
WNDPROC     g_origProc = nullptr;

typedef DWORD (WINAPI* XGetState_t)(DWORD, void*);
typedef DWORD (WINAPI* XSetState_t)(DWORD, void*);
typedef DWORD (WINAPI* XGetCaps_t)(DWORD, DWORD, void*);
typedef HWND  (WINAPI* GetWnd_t)();
XGetState_t g_realGetState = nullptr;
XSetState_t g_realSetState = nullptr;
XGetCaps_t  g_realGetCaps = nullptr;
GetWnd_t    g_realForeground = nullptr, g_realActive = nullptr;

void Log(const char* fmt, ...)
{
    char b[300];
    va_list a; va_start(a, fmt); _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a); va_end(a);
    Phase1_LogLine(b);
}

template <typename T> T* MapSection(const char* kind)
{
    char name[96];
    sprintf_s(name, "Local\\ac4pilot-%s-%u", kind, (unsigned)GetCurrentProcessId());
    HANDLE m = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)sizeof(T), name);
    if (!m) { Log("[Pilot] CreateFileMapping %s failed %lu\n", name, GetLastError()); return nullptr; }
    T* p = (T*)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(T));   // the handle stays open for the process
    if (p) memset(p, 0, sizeof(T));
    return p;
}

// ---- virtual pad ---------------------------------------------------------------------------------------------
// XINPUT_STATE = { DWORD dwPacketNumber; XINPUT_GAMEPAD { WORD wButtons; BYTE bLeftTrigger, bRightTrigger;
// SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } } = 16 bytes (Windows SDK xinput.h).
DWORD WINAPI FakeGetState(DWORD idx, void* st)
{
    if ((int)idx != g_padIndex || !g_pad)
        return g_realGetState ? g_realGetState(idx, st) : ERROR_DEVICE_NOT_CONNECTED;
    if (g_state) InterlockedIncrement((volatile LONG*)&g_state->padPolls);
    if (!st) return ERROR_SUCCESS;
    unsigned char* b = (unsigned char*)st;
    memset(b, 0, 16);
    if (g_pad->enabled) {
        *(DWORD*)(b + 0) = g_pad->seq;
        *(WORD*)(b + 4)  = g_pad->buttons;
        b[6] = g_pad->lt; b[7] = g_pad->rt;
        *(SHORT*)(b + 8)  = g_pad->lx; *(SHORT*)(b + 10) = g_pad->ly;
        *(SHORT*)(b + 12) = g_pad->rx; *(SHORT*)(b + 14) = g_pad->ry;
        if (g_state) InterlockedIncrement((volatile LONG*)&g_state->padServed);
    }
    return ERROR_SUCCESS;
}

DWORD WINAPI FakeSetState(DWORD idx, void* vib)
{
    if ((int)idx == g_padIndex && g_pad) return ERROR_SUCCESS;   // swallow rumble for the virtual pad
    return g_realSetState ? g_realSetState(idx, vib) : ERROR_DEVICE_NOT_CONNECTED;
}

// XINPUT_CAPABILITIES = { BYTE Type, SubType; WORD Flags; XINPUT_GAMEPAD Gamepad (12); XINPUT_VIBRATION (4) } = 20.
DWORD WINAPI FakeGetCaps(DWORD idx, DWORD flags, void* caps)
{
    if ((int)idx != g_padIndex || !g_pad)
        return g_realGetCaps ? g_realGetCaps(idx, flags, caps) : ERROR_DEVICE_NOT_CONNECTED;
    if (caps) {
        unsigned char* c = (unsigned char*)caps;
        memset(c, 0, 20);
        c[0] = 1;                       // XINPUT_DEVTYPE_GAMEPAD
        c[1] = 1;                       // XINPUT_DEVSUBTYPE_GAMEPAD
        *(WORD*)(c + 4) = 0xF3FF;       // every button
        c[6] = c[7] = 0xFF;
        *(SHORT*)(c + 8) = *(SHORT*)(c + 10) = *(SHORT*)(c + 12) = *(SHORT*)(c + 14) = (SHORT)0xFFC0;
        *(WORD*)(c + 16) = *(WORD*)(c + 18) = 0xFFFF;
    }
    return ERROR_SUCCESS;
}

HWND WINAPI FakeForeground() { return g_wnd ? g_wnd : (g_realForeground ? g_realForeground() : nullptr); }
HWND WINAPI FakeActive()     { return g_wnd ? g_wnd : (g_realActive ? g_realActive() : nullptr); }

// Replace the IAT entry that currently holds `real` (every import descriptor of the main module). Returns the count.
int PatchIat(void* real, void* repl)
{
    if (!real) return 0;
    int n = 0;
    __try {
        char* mod = (char*)GetModuleHandleA(nullptr);
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(mod + ((IMAGE_DOS_HEADER*)mod)->e_lfanew);
        IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(mod + dir.VirtualAddress); dir.VirtualAddress && imp->Name; ++imp) {
            for (IMAGE_THUNK_DATA* t = (IMAGE_THUNK_DATA*)(mod + imp->FirstThunk); t->u1.Function; ++t) {
                if ((void*)t->u1.Function != real) continue;
                DWORD old = 0;
                if (VirtualProtect(&t->u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &old)) {
                    t->u1.Function = (ULONG_PTR)repl;
                    VirtualProtect(&t->u1.Function, sizeof(ULONG_PTR), old, &old);
                    ++n;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { Log("[Pilot] IAT walk faulted\n"); }
    return n;
}

void TryPatchImports()
{
    HMODULE xi = GetModuleHandleA("xinput1_3.dll");
    if (!xi) return;                                              // retried from Poll
    g_realGetState = (XGetState_t)GetProcAddress(xi, MAKEINTRESOURCEA(2));
    g_realSetState = (XSetState_t)GetProcAddress(xi, MAKEINTRESOURCEA(3));
    g_realGetCaps  = (XGetCaps_t) GetProcAddress(xi, MAKEINTRESOURCEA(4));
    int a = PatchIat((void*)g_realGetState, (void*)&FakeGetState);
    int b = PatchIat((void*)g_realSetState, (void*)&FakeSetState);
    int c = PatchIat((void*)g_realGetCaps,  (void*)&FakeGetCaps);
    int d = 0, e = 0;
    if (g_keepFocus) {
        HMODULE u = GetModuleHandleA("user32.dll");
        g_realForeground = (GetWnd_t)GetProcAddress(u, "GetForegroundWindow");
        g_realActive     = (GetWnd_t)GetProcAddress(u, "GetActiveWindow");
        d = PatchIat((void*)g_realForeground, (void*)&FakeForeground);
        e = PatchIat((void*)g_realActive,     (void*)&FakeActive);
    }
    InterlockedExchange(&g_iatDone, 1);
    Log("[Pilot] imports patched: XInputGetState=%d SetState=%d GetCapabilities=%d GetForegroundWindow=%d GetActiveWindow=%d"
        " (pad index %d, keepFocus %d)\n", a, b, c, d, e, g_padIndex, g_keepFocus);
}

// ---- focus keeper -----------------------------------------------------------------------------------------------
LRESULT CALLBACK SubclassProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    bool drop = (msg == WM_ACTIVATEAPP && wp == FALSE) || (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) || msg == WM_KILLFOCUS;
    if (drop) {
        if (g_state) InterlockedIncrement((volatile LONG*)&g_state->focusDrops);
        return 0;
    }
    return CallWindowProcA(g_origProc, w, msg, wp, lp);
}

BOOL CALLBACK FindGameWindow(HWND w, LPARAM out)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) { *(HWND*)out = w; return FALSE; }
    return TRUE;
}

void TrySubclass()
{
    HWND w = nullptr;
    EnumWindows(FindGameWindow, (LPARAM)&w);
    if (!w) return;
    g_wnd = w;
    g_origProc = (WNDPROC)SetWindowLongPtrA(w, GWLP_WNDPROC, (LONG_PTR)&SubclassProc);
    InterlockedExchange(&g_focusDone, 1);
    Log("[Pilot] focus keeper on window %p (original proc %p)\n", (void*)w, (void*)g_origProc);
}

}  // namespace

void Init()
{
    g_enabled = GetPrivateProfileIntA("Pilot", "Enabled", 0, Paths::Ini());
    if (!g_enabled) return;
    if (GetPrivateProfileIntA("Coop", "VPadEnable", 0, Paths::Ini())) {
        g_enabled = 0;
        Log("[Pilot] refused: [Coop] VPadEnable=1 owns the XInputGetState import\n");
        return;
    }
    g_keepFocus = GetPrivateProfileIntA("Pilot", "KeepFocus", 0, Paths::Ini());
    g_padIndex  = GetPrivateProfileIntA("Pilot", "PadIndex", 0, Paths::Ini());
    if (g_padIndex < 0 || g_padIndex > 3) g_padIndex = 0;
    g_pad   = MapSection<PilotPad>("pad");
    g_state = MapSection<PilotState>("state");
    if (g_pad) g_pad->magic = 0x44344341;
    if (g_state) { g_state->magic = 0x53344341; g_state->version = 1; g_state->padIndex = (uint32_t)g_padIndex; }
    Log("[Pilot] on: pad section %p state section %p (sizeof state %u)\n", (void*)g_pad, (void*)g_state, (unsigned)sizeof(PilotState));
    TryPatchImports();
}

void Poll()
{
    if (!g_enabled) return;
    if (!g_iatDone) TryPatchImports();
    if (g_keepFocus && !g_focusDone) TrySubclass();
}

void OnPlayerFrame(void* human)
{
    if (!g_enabled || !g_state) return;
    PilotState* s = g_state;
    InterlockedIncrement((volatile LONG*)&s->seq);               // odd: writing
    __try {
        World* w = World::GetCurrentWorld();
        Entity* pl = w ? w->GetPlayer() : nullptr;
        s->world = (uint32_t)(uintptr_t)w;
        s->player = (uint32_t)(uintptr_t)pl;
        s->human = (uint32_t)(uintptr_t)human;
        if (pl) memcpy(s->m, &((BaseEntity*)pl)->m_GlobalMatrix, sizeof(s->m));
        uint32_t n = 0, total = 0;
        if (w) {
            total = (uint32_t)w->m_CharacterList.GetSize();
            for (uint32_t i = 0; i < total && n < kMaxChars; ++i) {
                Entity** slot = w->m_CharacterList.GetElement((int)i);
                Entity* e = slot ? *slot : nullptr;
                if (!e) continue;
                PilotChar& c = s->chars[n];
                memset(&c, 0, sizeof(c));
                c.ent = (uint32_t)(uintptr_t)e;
                const HookMath::Transform& m = ((BaseEntity*)e)->m_GlobalMatrix;
                c.pos[0] = m.M41; c.pos[1] = m.M42; c.pos[2] = m.M43;
                c.w9c = *(const uint32_t*)((const char*)e + 0x9C);
                c.aic = *(const uint32_t*)((const char*)e + 0xE8);
                c.rec = c.aic ? *(const uint32_t*)(uintptr_t)(c.aic + 0x98) : 0;
                c.status = c.rec ? *(const int32_t*)(uintptr_t)(c.rec + 0x2C) : -1;
                c.role   = c.rec ? *(const int32_t*)(uintptr_t)(c.rec + 0x18) : -1;
                c.flags = (e == pl) ? 1u : 0u;
                ++n;
            }
        }
        s->nChars = n;
        s->nTotal = total;
        s->tickMs = GetTickCount();
        ++s->frame;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int k = 0;
        if (k < 10) { ++k; Log("[Pilot] EXCEPTION 0x%08X in state write\n", GetExceptionCode()); }
    }
    InterlockedIncrement((volatile LONG*)&s->seq);               // even: stable
}

}  // namespace Pilot
