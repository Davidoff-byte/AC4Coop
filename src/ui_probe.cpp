// UI probe ([Coop] UiProbe=1, default 0): measures what an in-game ImGui overlay must hook, BEFORE building it.
//   - which present path fires: IDXGISwapChain::Present (d3d11/dxgi) vs IDirect3DDevice9::Present / EndScene,
//     and the swap chain's window, size and windowed flag;
//   - how the game reads input: IDirectInputDevice8 A/W calls (Acquire, GetDeviceState, GetDeviceData,
//     SetCooperativeLevel flags, which device), registered Raw Input devices, and which window messages
//     reach the game window (mouse, keyboard, char, WM_INPUT, WM_SETCURSOR).
// Method: vtable patching. A throwaway device of our own gives the address of the shared, static vtable in
// dxgi.dll / d3d9.dll / dinput8.dll; one entry is swapped for a counting stub that calls the saved original.
// No instruction is rewritten. Every 5 s one "[UiProbe]" line goes to net_log.txt-independent ui_probe.txt.
// Pure Win32/COM: no engine calls.
#define _CRT_SECURE_NO_WARNINGS
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3d9.h>
#include <dinput.h>
#include <cstdio>
#include "ui_probe.h"
#include "paths.h"

#include <process.h>
#pragma comment(lib, "dxguid.lib")   // GUID data only; the DLL entry points are resolved at run time below so
                                     // the ASI's import table does not change (it is loaded BY a dinput8 proxy)

namespace {

typedef HRESULT(WINAPI* D3D11CreateDeviceAndSwapChain_t)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT,
    UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef IDirect3D9* (WINAPI* Direct3DCreate9_t)(UINT);
typedef HRESULT(WINAPI* DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
FARPROC SysProc(const char* dll, const char* fn) {   // from System32, never the game folder's proxy
    char p[MAX_PATH]; GetSystemDirectoryA(p, MAX_PATH); lstrcatA(p, "\\"); lstrcatA(p, dll);
    HMODULE m = LoadLibraryA(p);
    return m ? GetProcAddress(m, fn) : nullptr;
}

FILE* g_f = nullptr;
void Log(const char* fmt, ...) {
    if (!g_f) g_f = _fsopen(Paths::LogPath("ui_probe.txt"), "a", 0x20);
    if (!g_f) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_f, fmt, ap); va_end(ap); fflush(g_f);
}

bool PatchSlot(void** vtbl, int slot, void* hook, void** orig) {
    DWORD old = 0;
    if (!VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_READWRITE, &old)) return false;
    *orig = vtbl[slot]; vtbl[slot] = hook;
    VirtualProtect(&vtbl[slot], sizeof(void*), old, &old);
    return true;
}

// ---- counters ----
volatile LONG c_present11 = 0, c_present9 = 0, c_endscene9 = 0;
volatile LONG c_acq = 0, c_state = 0, c_data = 0, c_coop = 0;
volatile LONG c_mm = 0, c_input = 0, c_kd = 0, c_char = 0, c_setcur = 0, c_lbd = 0;
IDXGISwapChain* volatile g_lastSwap = nullptr;
HWND g_gameWnd = nullptr;
WNDPROC g_origWndProc = nullptr;

typedef HRESULT(STDMETHODCALLTYPE* Present11_t)(IDXGISwapChain*, UINT, UINT);
Present11_t o_Present11 = nullptr;
HRESULT STDMETHODCALLTYPE h_Present11(IDXGISwapChain* sc, UINT sync, UINT flags) {
    InterlockedIncrement(&c_present11); g_lastSwap = sc;
    return o_Present11(sc, sync, flags);
}
typedef HRESULT(STDMETHODCALLTYPE* Present9_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
Present9_t o_Present9 = nullptr;
HRESULT STDMETHODCALLTYPE h_Present9(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
    InterlockedIncrement(&c_present9); return o_Present9(d, a, b, w, r);
}
typedef HRESULT(STDMETHODCALLTYPE* EndScene9_t)(IDirect3DDevice9*);
EndScene9_t o_EndScene9 = nullptr;
HRESULT STDMETHODCALLTYPE h_EndScene9(IDirect3DDevice9* d) { InterlockedIncrement(&c_endscene9); return o_EndScene9(d); }

// DirectInput: A and W devices have different vtables; both get the same counting stubs, with their own originals.
struct DiOrig { void* acquire; void* state; void* data; void* coop; };
DiOrig g_diA{}, g_diW{};
char g_coopLog[512] = {0};
template <DiOrig* O>
struct DiHooks {
    static HRESULT STDMETHODCALLTYPE Acquire(IDirectInputDevice8A* d) {
        InterlockedIncrement(&c_acq); return ((HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A*))O->acquire)(d);
    }
    static HRESULT STDMETHODCALLTYPE GetState(IDirectInputDevice8A* d, DWORD n, LPVOID p) {
        InterlockedIncrement(&c_state); return ((HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A*, DWORD, LPVOID))O->state)(d, n, p);
    }
    static HRESULT STDMETHODCALLTYPE GetData(IDirectInputDevice8A* d, DWORD cb, LPDIDEVICEOBJECTDATA od, LPDWORD n, DWORD f) {
        InterlockedIncrement(&c_data);
        return ((HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD))O->data)(d, cb, od, n, f);
    }
    static HRESULT STDMETHODCALLTYPE Coop(IDirectInputDevice8A* d, HWND w, DWORD flags) {
        InterlockedIncrement(&c_coop);
        DIDEVICEINSTANCEA di{}; di.dwSize = sizeof(di);
        char name[64] = "?";
        if (SUCCEEDED(d->GetDeviceInfo(&di))) lstrcpynA(name, di.tszInstanceName, sizeof(name));
        Log("[UiProbe] SetCooperativeLevel(%s) dev=%p '%s' type=0x%02x hwnd=%p flags=0x%lx%s%s%s%s%s\n",
            O == &g_diA ? "A" : "W", (void*)d, name, (unsigned)(di.dwDevType & 0xFF), (void*)w, flags,
            flags & DISCL_EXCLUSIVE ? " EXCLUSIVE" : "", flags & DISCL_NONEXCLUSIVE ? " NONEXCLUSIVE" : "",
            flags & DISCL_FOREGROUND ? " FOREGROUND" : "", flags & DISCL_BACKGROUND ? " BACKGROUND" : "",
            flags & DISCL_NOWINKEY ? " NOWINKEY" : "");
        return ((HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A*, HWND, DWORD))O->coop)(d, w, flags);
    }
};

LRESULT CALLBACK ProbeWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_MOUSEMOVE: InterlockedIncrement(&c_mm); break;
        case WM_INPUT: InterlockedIncrement(&c_input); break;
        case WM_KEYDOWN: case WM_SYSKEYDOWN: InterlockedIncrement(&c_kd); break;
        case WM_CHAR: InterlockedIncrement(&c_char); break;
        case WM_SETCURSOR: InterlockedIncrement(&c_setcur); break;
        case WM_LBUTTONDOWN: InterlockedIncrement(&c_lbd); break;
    }
    return CallWindowProcA(g_origWndProc, h, m, w, l);
}

HWND Dummy() {
    WNDCLASSA wc{}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "AC4CoopUiProbeDummy";
    RegisterClassA(&wc);
    return CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
}

void HookD3D11(HWND dummy) {
    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount = 1; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = dummy; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    auto create = (D3D11CreateDeviceAndSwapChain_t)SysProc("d3d11.dll", "D3D11CreateDeviceAndSwapChain");
    if (!create) { Log("[UiProbe] d3d11 entry point not found\n"); return; }
    HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                               &sd, &sc, &dev, nullptr, &ctx);
    if (FAILED(hr)) { Log("[UiProbe] d3d11 dummy failed hr=0x%08lx\n", hr); return; }
    void** vt = *(void***)sc;
    const bool ok = PatchSlot(vt, 8, (void*)&h_Present11, (void**)&o_Present11);   // IDXGISwapChain::Present
    Log("[UiProbe] dxgi Present vtable=%p slot8=%p patched=%d\n", (void*)vt, (void*)o_Present11, ok ? 1 : 0);
    sc->Release(); ctx->Release(); dev->Release();   // the vtable is static in dxgi.dll; the patch stays
}

void HookD3D9(HWND dummy) {
    auto create9 = (Direct3DCreate9_t)SysProc("d3d9.dll", "Direct3DCreate9");
    IDirect3D9* d3d = create9 ? create9(D3D_SDK_VERSION) : nullptr;
    if (!d3d) { Log("[UiProbe] d3d9 create failed\n"); return; }
    D3DPRESENT_PARAMETERS pp{}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = dummy;
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, dummy, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) { Log("[UiProbe] d3d9 dummy device failed hr=0x%08lx\n", hr); d3d->Release(); return; }
    void** vt = *(void***)dev;
    const bool a = PatchSlot(vt, 17, (void*)&h_Present9, (void**)&o_Present9);       // IDirect3DDevice9::Present
    const bool b = PatchSlot(vt, 42, (void*)&h_EndScene9, (void**)&o_EndScene9);     // IDirect3DDevice9::EndScene
    Log("[UiProbe] d3d9 device vtable=%p patched present=%d endscene=%d\n", (void*)vt, a ? 1 : 0, b ? 1 : 0);
    dev->Release(); d3d->Release();
}

template <DiOrig* O>
void HookDiVtable(void** vt, const char* tag) {
    const bool a = PatchSlot(vt, 7, (void*)&DiHooks<O>::Acquire, &O->acquire);
    const bool b = PatchSlot(vt, 9, (void*)&DiHooks<O>::GetState, &O->state);
    const bool c = PatchSlot(vt, 10, (void*)&DiHooks<O>::GetData, &O->data);
    const bool d = PatchSlot(vt, 13, (void*)&DiHooks<O>::Coop, &O->coop);
    Log("[UiProbe] dinput %s device vtable=%p patched=%d%d%d%d\n", tag, (void*)vt, a, b, c, d);
}

void HookDInput() {
    HMODULE self = GetModuleHandleA(nullptr);
    auto DirectInput8Create = (DirectInput8Create_t)SysProc("dinput8.dll", "DirectInput8Create");
    if (!DirectInput8Create) { Log("[UiProbe] DirectInput8Create not found\n"); return; }
    IDirectInput8A* diA = nullptr; IDirectInput8W* diW = nullptr;
    if (SUCCEEDED(DirectInput8Create(self, DIRECTINPUT_VERSION, IID_IDirectInput8A, (void**)&diA, nullptr))) {
        IDirectInputDevice8A* m = nullptr;
        if (SUCCEEDED(diA->CreateDevice(GUID_SysKeyboard, &m, nullptr))) { HookDiVtable<&g_diA>(*(void***)m, "A"); m->Release(); }
        diA->Release();
    } else Log("[UiProbe] DirectInput8Create A failed\n");
    if (SUCCEEDED(DirectInput8Create(self, DIRECTINPUT_VERSION, IID_IDirectInput8W, (void**)&diW, nullptr))) {
        IDirectInputDevice8W* m = nullptr;
        if (SUCCEEDED(diW->CreateDevice(GUID_SysKeyboard, &m, nullptr))) {
            void** vt = *(void***)m;
            if (vt == (void**)nullptr) Log("[UiProbe] W vtable null\n");
            else if (g_diA.acquire && vt[7] == (void*)&DiHooks<&g_diA>::Acquire) Log("[UiProbe] dinput W shares the A vtable\n");
            else HookDiVtable<&g_diW>(vt, "W");
            m->Release();
        }
        diW->Release();
    } else Log("[UiProbe] DirectInput8Create W failed\n");
}

struct FindCtx { DWORD pid; HWND best; LONG area; };
BOOL CALLBACK FindProc(HWND h, LPARAM lp) {
    FindCtx* c = (FindCtx*)lp; DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    char cls[64] = {0}; GetClassNameA(h, cls, sizeof(cls));
    if (pid != c->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER) || !strcmp(cls, "AC4CoopLobbyPanel")) return TRUE;
    RECT r; GetWindowRect(h, &r); const LONG a = (r.right - r.left) * (r.bottom - r.top);
    if (a > c->area) { c->area = a; c->best = h; }
    return TRUE;
}

unsigned __stdcall ProbeThread(void*) {
    Log("[UiProbe] ---- start pid=%lu ----\n", GetCurrentProcessId());
    // DirectInput first and at once: SetCooperativeLevel runs once when the game sets up input, so a late
    // hook would miss it. The D3D dummies wait 10 s so our throwaway devices are not created while the game
    // is creating its own (they are released right away; only the vtable patch stays).
    HookDInput();
    Sleep(10000);
    HWND dummy = Dummy();
    HookD3D11(dummy); HookD3D9(dummy);
    DestroyWindow(dummy);
    for (int tick = 0;; ++tick) {
        Sleep(5000);
        if (!g_gameWnd) {
            FindCtx c{ GetCurrentProcessId(), nullptr, 0 }; EnumWindows(FindProc, (LPARAM)&c);
            if (c.best) {
                g_gameWnd = c.best;
                g_origWndProc = (WNDPROC)SetWindowLongPtrA(g_gameWnd, GWLP_WNDPROC, (LONG_PTR)&ProbeWndProc);
                char cls[64] = {0}; GetClassNameA(g_gameWnd, cls, sizeof(cls));
                Log("[UiProbe] game window %p class '%s' subclassed=%d unicode=%d\n", (void*)g_gameWnd, cls,
                    g_origWndProc ? 1 : 0, IsWindowUnicode(g_gameWnd) ? 1 : 0);
                RAWINPUTDEVICE rid[16]; UINT n = 16;
                const UINT got = GetRegisteredRawInputDevices(rid, &n, sizeof(RAWINPUTDEVICE));
                Log("[UiProbe] raw input devices registered: %d\n", (int)got);
                for (UINT i = 0; i < got && got != (UINT)-1; ++i)
                    Log("[UiProbe]   page=0x%x usage=0x%x flags=0x%lx hwnd=%p\n", rid[i].usUsagePage, rid[i].usUsage, rid[i].dwFlags, (void*)rid[i].hwndTarget);
            }
        }
        IDXGISwapChain* sc = g_lastSwap;
        char scInfo[160] = "-";
        if (sc && (tick % 6) == 0) {
            DXGI_SWAP_CHAIN_DESC d{};
            if (SUCCEEDED(sc->GetDesc(&d)))
                _snprintf(scInfo, sizeof(scInfo), "swap=%p hwnd=%p %ux%u fmt=%d windowed=%d buffers=%u", (void*)sc, (void*)d.OutputWindow,
                          d.BufferDesc.Width, d.BufferDesc.Height, (int)d.BufferDesc.Format, d.Windowed, d.BufferCount);
        }
        CURSORINFO ci{}; ci.cbSize = sizeof(ci); GetCursorInfo(&ci);
        HWND fg = GetForegroundWindow();
        Log("[UiProbe] t=%ds present11=%ld present9=%ld endscene9=%ld | di acquire=%ld getstate=%ld getdata=%ld coop=%ld | "
            "wm mousemove=%ld input=%ld keydown=%ld char=%ld setcursor=%ld lbutton=%ld | fg_is_game=%d cursor_showing=%d pos=%ld,%ld | %s\n",
            (tick + 1) * 5, c_present11, c_present9, c_endscene9, c_acq, c_state, c_data, c_coop,
            c_mm, c_input, c_kd, c_char, c_setcur, c_lbd, fg == g_gameWnd ? 1 : 0, (ci.flags & CURSOR_SHOWING) ? 1 : 0, ci.ptScreenPos.x, ci.ptScreenPos.y, scInfo);
    }
}

}  // namespace

namespace UiProbe {
void Start() {
    if (!GetPrivateProfileIntA("Coop", "UiProbe", 0, Paths::Ini())) return;
    _beginthreadex(nullptr, 0, ProbeThread, nullptr, 0, nullptr);   // CRT in the thread: _beginthreadex
}
}
