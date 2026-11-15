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
static_assert(offsetof(Pilot::PilotCtl, f) == 20 && offsetof(Pilot::PilotCtl, rcount) == 100 && sizeof(Pilot::PilotCtl) == 1140, "PilotCtl layout changed: update tools/pilot.py");

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

// ---- world-space intent: IHumanGround slots 0/1 (intention-seam.md seam 1) ----------------------------------------------
// vtable 0x026E7DC0: slot 0 = 0x01CB91B0 SetDirection(const Vector4*) (mov ecx,[ecx-4]; jmp sub_56BDD0: movaps [ecx+20h]),
// slot 1 = 0x01D51690 SetSpeedRatio(float) (writes HumanGroundData+0x98, zeroes scope+0x5F8); both thiscall, retn 4.
// Values re-read in db a3 (job k13: bytes B0 91 CB 01 / 90 16 D5 01, slot 1 decompiled). Entity of an IHumanGround* p:
// [[[p-0xC]+0x108]+4]; HumanGroundData = [p-4], m_DestHeading at +0x20 (16-byte aligned in the game's own movaps store).
struct alignas(16) V4i { float x, y, z, w; };
typedef void (__thiscall* SetDir_t)(void*, const V4i*);
typedef void (__thiscall* SetSpeed_t)(void*, float);
SetDir_t   g_origSetDir = nullptr;
SetSpeed_t g_origSetSpeed = nullptr;
volatile uint32_t g_intentEntity = 0;       // the player entity, refreshed every player frame
alignas(16) V4i g_intentDir = { 0, 1, 0, 0 };
volatile float g_intentSpeed = 0.0f;
volatile DWORD g_intentUntil = 0;           // GetTickCount() deadline; 0 = no intent

bool IntentFor(void* ig)
{
    if (!g_intentUntil || (LONG)(GetTickCount() - g_intentUntil) > 0) return false;
    uint32_t a = *(const uint32_t*)((const char*)ig - 0xC);
    if (!a) return false;
    uint32_t h = *(const uint32_t*)(uintptr_t)(a + 0x108);
    return h && *(const uint32_t*)(uintptr_t)(h + 4) == g_intentEntity;
}

void __fastcall HookSetDir(void* ig, void* /*edx*/, const V4i* dir)
{
    if (IntentFor(ig)) {
        alignas(16) V4i d = g_intentDir;
        if (g_ctl) InterlockedIncrement((volatile LONG*)&g_ctl->reserved[2]);
        g_origSetDir(ig, &d);
        return;
    }
    g_origSetDir(ig, dir);
}

void __fastcall HookSetSpeed(void* ig, void* /*edx*/, float r)
{
    if (IntentFor(ig)) {
        g_origSetSpeed(ig, g_intentSpeed);
        // The handler calls SetDirection only when its own speed >= 0.01; with the stick idle it never would, so store
        // the direction here exactly as slot 0 does (HumanGroundData = [p-4], m_DestHeading +0x20).
        char* data = *(char**)((char*)ig - 4);
        if (data) { alignas(16) V4i d = g_intentDir; memcpy(data + 0x20, &d, sizeof(d)); }
        if (g_ctl) InterlockedIncrement((volatile LONG*)&g_ctl->reserved[3]);
        return;
    }
    g_origSetSpeed(ig, r);
}

void InstallIntentHooks()
{
    uint32_t* slot = (uint32_t*)0x026E7DC0;
    if (slot[0] != 0x01CB91B0u || slot[1] != 0x01D51690u) {
        Log("[Pilot] intent hooks NOT installed: vtable 0x026E7DC0 holds %08X %08X (expected 01CB91B0 01D51690)\n", slot[0], slot[1]);
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) { Log("[Pilot] intent hooks: VirtualProtect failed\n"); return; }
    g_origSetDir = (SetDir_t)(uintptr_t)slot[0];
    g_origSetSpeed = (SetSpeed_t)(uintptr_t)slot[1];
    InterlockedExchange((volatile LONG*)&slot[0], (LONG)(uintptr_t)&HookSetDir);
    InterlockedExchange((volatile LONG*)&slot[1], (LONG)(uintptr_t)&HookSetSpeed);
    VirtualProtect(slot, 8, old, &old);
    if (g_ctl) g_ctl->reserved[4] = 1;
    Log("[Pilot] intent hooks installed on IHumanGround vtable 0x026E7DC0 slots 0/1\n");
}

// ---- climbing intent: Climbing context interface vtable 0x026D4588 slots 37/38 ---------------------------------------
// bf-coop ref/AC4Coop-lobby/docs/evidence/climb-decision-leads-2026-11-15.md item 13 (run 8744): slot 37 0x01537F30
// SetDirection2D(const Vector2*) stores [this-4]+0x0C/+0x10, slot 38 0x0152B0C0 SetSpeedRatio(float) clamps 0..1 to
// [this-4]+0x14; the player's climbing handler sub_113DAC0 calls both every update. The interface sits at context+0x10
// (ctor sub_1581770), context+4 = Human, so entity = [[[p-0xC]+0x108]+4] exactly as for IHumanGround. Both thiscall,
// retn 4 (slot 37: `mov eax,[ecx-4]` ... `retn 4`, 24 bytes; slot 38: 44 bytes).
typedef void (__thiscall* ClimbDir_t)(void*, const float*);
typedef void (__thiscall* ClimbSpd_t)(void*, float);
ClimbDir_t g_origClimbDir = nullptr;
ClimbSpd_t g_origClimbSpd = nullptr;
float g_climbX = 0, g_climbY = 0, g_climbSpeed = 1.0f;
volatile DWORD g_climbUntil = 0;

bool ClimbIntentFor(void* ig)
{
    if (!g_climbUntil || (LONG)(GetTickCount() - g_climbUntil) > 0) return false;
    uint32_t a = *(const uint32_t*)((const char*)ig - 0xC);
    if (!a) return false;
    uint32_t h = *(const uint32_t*)(uintptr_t)(a + 0x108);
    return h && *(const uint32_t*)(uintptr_t)(h + 4) == g_intentEntity;
}

void __fastcall HookClimbDir(void* ig, void* /*edx*/, const float* v)
{
    if (ClimbIntentFor(ig)) {
        float d[2] = { g_climbX, g_climbY };
        if (g_ctl) InterlockedIncrement((volatile LONG*)&g_ctl->reserved[5]);
        g_origClimbDir(ig, d);
        return;
    }
    g_origClimbDir(ig, v);
}

void __fastcall HookClimbSpd(void* ig, void* /*edx*/, float r)
{
    if (ClimbIntentFor(ig)) {
        if (g_ctl) InterlockedIncrement((volatile LONG*)&g_ctl->reserved[6]);
        g_origClimbSpd(ig, g_climbSpeed);
        return;
    }
    g_origClimbSpd(ig, r);
}

void InstallClimbIntentHooks()
{
    uint32_t* slot = (uint32_t*)(0x026D4588 + 37 * 4);
    if (slot[0] != 0x01537F30u || slot[1] != 0x0152B0C0u) {
        Log("[Pilot] climb intent hooks NOT installed: vtable 0x026D4588 slots 37/38 hold %08X %08X (expected 01537F30 0152B0C0)\n", slot[0], slot[1]);
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) { Log("[Pilot] climb intent hooks: VirtualProtect failed\n"); return; }
    g_origClimbDir = (ClimbDir_t)(uintptr_t)slot[0];
    g_origClimbSpd = (ClimbSpd_t)(uintptr_t)slot[1];
    InterlockedExchange((volatile LONG*)&slot[0], (LONG)(uintptr_t)&HookClimbDir);
    InterlockedExchange((volatile LONG*)&slot[1], (LONG)(uintptr_t)&HookClimbSpd);
    VirtualProtect(slot, 8, old, &old);
    if (g_ctl) g_ctl->reserved[7] = 1;
    Log("[Pilot] climb intent hooks installed on vtable 0x026D4588 slots 37/38\n");
}

void SetIntent(float dx, float dy, float speed, DWORD holdMs)
{
    float l = sqrtf(dx * dx + dy * dy);
    if (l < 1e-4f) { g_intentUntil = 0; return; }
    g_intentDir.x = dx / l; g_intentDir.y = dy / l; g_intentDir.z = 0; g_intentDir.w = 0;
    g_intentSpeed = speed < 0 ? 0 : speed > 1 ? 1 : speed;
    g_intentUntil = GetTickCount() + (holdMs ? holdMs : 1);
}

// In-process goto, steered on every player frame.
struct GotoState { bool on; float tx, ty, speed, tol, stuckSec, best; DWORD bestTick; };
GotoState g_goto = {};

void GotoTick(Entity* pl)
{
    if (!g_goto.on || !pl || !g_ctl) return;
    const HookMath::Transform& m = ((BaseEntity*)pl)->m_GlobalMatrix;
    float dx = g_goto.tx - m.M41, dy = g_goto.ty - m.M42, d = sqrtf(dx * dx + dy * dy);
    memcpy(&g_ctl->reserved[1], &d, 4);
    if (d <= g_goto.tol) { g_goto.on = false; g_intentUntil = 0; g_ctl->reserved[0] = 2; return; }
    DWORD now = GetTickCount();
    if (d < g_goto.best - 0.3f) { g_goto.best = d; g_goto.bestTick = now; }
    else if ((now - g_goto.bestTick) > (DWORD)(g_goto.stuckSec * 1000.0f)) { g_goto.on = false; g_intentUntil = 0; g_ctl->reserved[0] = 3; return; }
    float sp = g_goto.speed;
    if (d < 2.0f && sp > 0.5f) sp = 0.5f;                       // ease in to avoid overshoot
    SetIntent(dx, dy, sp, 250);
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
    } else if (c->cmd == kCmdIntent) {
        if (!g_origSetDir) status = -4;
        else { g_goto.on = false; SetIntent(c->f[0], c->f[1], c->f[2], c->f[3] > 0 ? (DWORD)c->f[3] : 300); }
    } else if (c->cmd == kCmdGoto) {
        if (!g_origSetDir) status = -4;
        else {
            g_goto.on = true; g_goto.tx = c->f[0]; g_goto.ty = c->f[1];
            g_goto.speed = c->f[2] > 0 ? c->f[2] : 1.0f; g_goto.tol = c->f[3] > 0 ? c->f[3] : 1.0f;
            g_goto.stuckSec = c->f[4] > 0 ? c->f[4] : 3.0f; g_goto.best = 1e9f; g_goto.bestTick = GetTickCount();
            c->reserved[0] = 1;
        }
    } else if (c->cmd == kCmdClimbIntent) {
        if (!g_origClimbDir) status = -4;
        else {
            g_climbX = c->f[0]; g_climbY = c->f[1]; g_climbSpeed = c->f[2] < 0 ? 0 : c->f[2] > 1 ? 1 : c->f[2];
            g_climbUntil = GetTickCount() + (c->f[3] > 0 ? (DWORD)c->f[3] : 300);
        }
    } else if (c->cmd == kCmdStop) {
        g_goto.on = false; g_intentUntil = 0; g_climbUntil = 0; if (c->reserved[0] == 1) c->reserved[0] = 4;
    } else if (c->cmd >= kCmdRay && c->cmd <= kCmdSense) {
        if (!pl) status = -1;
        else { __try { status = RunWorldCommand(c, pl); } __except (EXCEPTION_EXECUTE_HANDLER) { status = -2; } }
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
    if (GetPrivateProfileIntA("Pilot", "Intent", 1, Paths::Ini())) InstallIntentHooks();   // [Pilot] Intent (default 1 under Enabled)
    if (GetPrivateProfileIntA("Pilot", "ClimbIntent", 1, Paths::Ini())) InstallClimbIntentHooks();   // [Pilot] ClimbIntent (default 1 under Enabled)
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
        g_intentEntity = (uint32_t)(uintptr_t)pl;
        RunCommand(pl);
        GotoTick(pl);
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
