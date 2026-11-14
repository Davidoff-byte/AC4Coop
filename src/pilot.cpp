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
#include <d3d11.h>
#include <cmath>
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
static_assert(offsetof(Pilot::PilotCtl, f) == 20 && sizeof(Pilot::PilotCtl) == 100, "PilotCtl layout changed: update tools/pilot.py");

namespace Pilot {
namespace {

int g_enabled = 0, g_keepFocus = 0, g_padIndex = 0;
volatile LONG g_iatDone = 0, g_focusDone = 0;
PilotPad*   g_pad = nullptr;
PilotState* g_state = nullptr;
PilotCtl*   g_ctl = nullptr;
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

// ---- frame limiter (AU-RW focus.cpp presentFake, adapted) -----------------------------------------------------------
typedef HRESULT (STDMETHODCALLTYPE* Present_t)(IDXGISwapChain*, UINT, UINT);
Present_t     g_realPresent = nullptr;
void**        g_presentTable = nullptr;
LARGE_INTEGER g_qpcFreq = {};
int64_t       g_nextFrame = 0;
LONG          g_fpsWindowPresents = 0;
DWORD         g_fpsWindowStart = 0;
volatile LONG g_presentTried = 0;

HRESULT STDMETHODCALLTYPE FakePresent(IDXGISwapChain* chain, UINT sync, UINT flags)
{
    PilotCtl* c = g_ctl;
    if (c && c->fpsCap && !(flags & DXGI_PRESENT_TEST)) {
        const int64_t period = g_qpcFreq.QuadPart / c->fpsCap;
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        if (!g_nextFrame || now.QuadPart - g_nextFrame > period) g_nextFrame = now.QuadPart;   // start, or fell behind
        while (now.QuadPart < g_nextFrame) {
            int64_t ms = (g_nextFrame - now.QuadPart) * 1000 / g_qpcFreq.QuadPart;
            if (ms > 1) Sleep((DWORD)(ms - 1)); else YieldProcessor();
            QueryPerformanceCounter(&now);
        }
        g_nextFrame += period;
    }
    if (c) {
        InterlockedIncrement((volatile LONG*)&c->presents);
        ++g_fpsWindowPresents;
        DWORD t = GetTickCount();
        if (t - g_fpsWindowStart >= 1000) { c->fps = (uint32_t)(g_fpsWindowPresents * 1000 / (LONG)(t - g_fpsWindowStart + 1)); g_fpsWindowPresents = 0; g_fpsWindowStart = t; }
    }
    return g_realPresent(chain, sync, flags);
}

void TryPatchPresent()
{
    if (InterlockedExchange(&g_presentTried, 1)) return;
    QueryPerformanceFrequency(&g_qpcFreq);
    HMODULE d3d = GetModuleHandleA("d3d11.dll");
    typedef HRESULT (WINAPI* Create_t)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                      const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    Create_t create = d3d ? (Create_t)GetProcAddress(d3d, "D3D11CreateDeviceAndSwapChain") : nullptr;
    if (!create) { Log("[Pilot] frame cap: d3d11.dll not loaded\n"); return; }
    HWND dummy = CreateWindowExA(0, "STATIC", "ac4pilot-dummy", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
    if (!dummy) return;
    DXGI_SWAP_CHAIN_DESC d = {};
    d.BufferCount = 1; d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.BufferDesc.Width = d.BufferDesc.Height = 64;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.OutputWindow = dummy; d.SampleDesc.Count = 1; d.Windowed = TRUE;
    IDXGISwapChain* chain = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    if (SUCCEEDED(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d, &chain, &dev, nullptr, &ctx)) && chain) {
        void** table = *(void***)chain;
        DWORD old;
        if (VirtualProtect(table + 8, sizeof(void*), PAGE_READWRITE, &old)) {
            g_presentTable = table;
            g_realPresent = (Present_t)table[8];
            table[8] = (void*)&FakePresent;
            VirtualProtect(table + 8, sizeof(void*), old, &old);
            if (g_ctl) g_ctl->presentPatched = 1;
        }
    }
    if (chain) chain->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(dummy);
    Log("[Pilot] frame cap: Present slot %p patched=%d (original %p), cap %u\n", (void*)(g_presentTable ? g_presentTable + 8 : nullptr),
        g_realPresent ? 1 : 0, (void*)g_realPresent, g_ctl ? g_ctl->fpsCap : 0);
}

// ---- commands executed on the game thread (player frame) ------------------------------------------------------------
void RunCommand(Entity* pl)
{
    PilotCtl* c = g_ctl;
    if (!c || c->req == c->done) return;
    int32_t status = 0;
    if (c->cmd == kCmdTeleport) {
        if (!pl) status = -1;
        else {
            __try {
                HookMath::Transform m = ((BaseEntity*)pl)->m_GlobalMatrix;
                m.M41 = c->f[0]; m.M42 = c->f[1]; m.M43 = c->f[2];
                if (!(c->f[3] != c->f[3])) {
                    // UNVERIFIED (one live observation, docs/evidence/pilot-live-2026-11-15.md): row 1 is the facing and
                    // row 0 = row1 x up gives a right-handed frame: row0 = (sin y, -cos y, 0), row1 = (cos y, sin y, 0).
                    float y = c->f[3], sy = sinf(y), cy = cosf(y);
                    m.M11 = sy;  m.M12 = -cy; m.M13 = 0.0f;
                    m.M21 = cy;  m.M22 = sy;  m.M23 = 0.0f;
                    m.M31 = 0.0f; m.M32 = 0.0f; m.M33 = 1.0f;
                }
                pl->SetGlobalMatrixInternal(&m, true);
                Log("[Pilot] teleport -> (%.2f, %.2f, %.2f) yaw %s\n", c->f[0], c->f[1], c->f[2], (c->f[3] != c->f[3]) ? "kept" : "set");
            } __except (EXCEPTION_EXECUTE_HANDLER) { status = -2; }
        }
    } else if (c->cmd == kCmdFpsCap) {
        float v = c->f[0];
        c->fpsCap = (v <= 0.0f) ? 0u : (uint32_t)(v > 240.0f ? 240.0f : v);
        status = g_realPresent ? 0 : -4;
    } else status = -3;
    c->status = status;
    c->done = c->req;
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
    g_ctl   = MapSection<PilotCtl>("ctl");
    if (g_pad) g_pad->magic = 0x44344341;
    if (g_ctl) {
        g_ctl->magic = 0x43344341;
        int cap = GetPrivateProfileIntA("Pilot", "FpsCap", 0, Paths::Ini());
        g_ctl->fpsCap = (uint32_t)(cap < 0 ? 0 : cap > 240 ? 240 : cap);
    }
    if (g_state) { g_state->magic = 0x53344341; g_state->version = 1; g_state->padIndex = (uint32_t)g_padIndex; }
    Log("[Pilot] on: pad section %p state section %p (sizeof state %u)\n", (void*)g_pad, (void*)g_state, (unsigned)sizeof(PilotState));
    TryPatchImports();
}

void Poll()
{
    if (!g_enabled) return;
    if (!g_iatDone) TryPatchImports();
    if (g_keepFocus && !g_focusDone) TrySubclass();
    // Present is patched only once a cap is wanted ([Pilot] FpsCap or the fps command), never speculatively at start.
    if (!g_presentTried && g_ctl && (g_ctl->fpsCap || (g_ctl->req != g_ctl->done && g_ctl->cmd == kCmdFpsCap)) && GetModuleHandleA("d3d11.dll")) TryPatchPresent();
    if (g_ctl && g_ctl->req != g_ctl->done && g_ctl->cmd == kCmdFpsCap) RunCommand(nullptr);   // works in menus too
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
        RunCommand(pl);
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
