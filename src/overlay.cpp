// In-game overlay hook layer (see overlay.h for the verified capture path). Pure Win32/COM: no engine calls.
#define _CRT_SECURE_NO_WARNINGS
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dinput.h>
#include <process.h>
#include <cstdio>
#include <cstring>
#include <share.h>
#include "overlay.h"
#include "overlay_ui.h"
#include "paths.h"
#include "net.h"

// phase1.cpp: the co-op role ([Overlay] RoleFromPanel). Set once per process; -1 = not picked yet.
bool Phase1_SetEntityHost(int host);
int  Phase1_EntityHostState();
#pragma comment(lib, "dxguid.lib")

namespace {

// ---- log: overlay_log.txt next to the other logs -------------------------------------------------------------
// Opened, appended and closed per line, non-inheritable ("N"), sharing read+write. Measured 2026-11-15 17:34: a
// long-lived deny-write handle opened by a store hand-off process was inherited by UbisoftGameLauncher.exe (the real
// game's parent), which then held overlay_log.txt for the whole session, so every line of the real game was lost.
CRITICAL_SECTION g_logCs; bool g_logInit = false;
void Log(const char* fmt, ...) {
    if (!g_logInit) return;
    EnterCriticalSection(&g_logCs);
    if (FILE* f = _fsopen(Paths::LogPath("overlay_log.txt"), "aN", _SH_DENYNO)) {
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(f, "%02d:%02d:%02d.%03d [%lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId());
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fclose(f);
    }
    LeaveCriticalSection(&g_logCs);
}
void LogLine(const char* s) { Log("%s", s); }

bool PatchSlot(void** vtbl, int slot, void* hook, void** orig) {
    DWORD old = 0;
    if (!VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    *orig = vtbl[slot]; vtbl[slot] = hook;
    VirtualProtect(&vtbl[slot], sizeof(void*), old, &old);
    return true;
}

// ---- config / shared state ---------------------------------------------------------------------------------------
bool g_probe = false, g_enable = false;
int  g_key = VK_F10;
CRITICAL_SECTION g_cs;                   // guards everything below that two threads touch
volatile LONG c_present = 0, c_scCreated = 0, c_resize = 0, c_wmMouse = 0, c_wmKey = 0, c_diState = 0, c_diData = 0, c_diZeroed = 0;
IDXGISwapChain* volatile g_sc = nullptr; // the game's swap chain (from the CreateSwapChain wrap)
HWND g_hwnd = nullptr;
WNDPROC g_origWndProc = nullptr;
volatile bool g_open = false;            // panel visible (read by the input hooks on other threads)
bool g_uiReady = false, g_uiFailed = false;
ID3D11RenderTargetView* g_rtv = nullptr;
bool g_sawWmMouse = false;               // per open: real WM_MOUSEMOVE arrived -> do not drive the cursor from DirectInput

struct QMsg { UINT m; WPARAM w; LPARAM l; };
QMsg g_q[256]; int g_qn = 0;             // window messages for the panel, drained on the render thread
int  g_diDx = 0, g_diDy = 0; BYTE g_diBtn[3] = {0}; bool g_diBtnSeen = false;   // DirectInput mouse while open
OverlayUi::Status g_status{}; bool g_statusDirty = false;

// ---- DXGI hooks ----------------------------------------------------------------------------------------------------
typedef HRESULT(WINAPI* CreateDXGIFactory1_t)(REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE* CreateSwapChain_t)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* ResizeBuffers_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
CreateDXGIFactory1_t o_CreateFactory1 = nullptr;
CreateSwapChain_t o_CreateSwapChain = nullptr;
Present_t o_Present = nullptr;
ResizeBuffers_t o_ResizeBuffers = nullptr;
void** const kIatCreateDXGIFactory1 = (void**)0x01E3A618;   // IDA: import dxgi!CreateDXGIFactory1, sole user thunk 0x976220

HRESULT STDMETHODCALLTYPE h_Present(IDXGISwapChain* sc, UINT sync, UINT flags);
HRESULT STDMETHODCALLTYPE h_ResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl);

HRESULT STDMETHODCALLTYPE h_CreateSwapChain(IDXGIFactory* fac, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out) {
    const HRESULT hr = o_CreateSwapChain(fac, dev, d, out);
    InterlockedIncrement(&c_scCreated);
    if (SUCCEEDED(hr) && out && *out) {
        IDXGISwapChain* sc = *out;
        if (!o_Present) {   // the vtable is static in dxgi.dll: patch once
            void** vt = *(void***)sc;
            const bool a = PatchSlot(vt, 8, (void*)&h_Present, (void**)&o_Present);           // IDXGISwapChain::Present
            const bool b = PatchSlot(vt, 13, (void*)&h_ResizeBuffers, (void**)&o_ResizeBuffers); // IDXGISwapChain::ResizeBuffers
            Log("[Overlay] swap chain vtable %p patched present=%d resize=%d\n", (void*)vt, a, b);
        }
        g_sc = sc;
        if (d) { g_hwnd = d->OutputWindow;
            Log("[Overlay] game swap chain %p hwnd=%p %ux%u fmt=%d buffers=%u windowed=%d swapeffect=%d flags=0x%x\n", (void*)sc, (void*)d->OutputWindow,
                d->BufferDesc.Width, d->BufferDesc.Height, (int)d->BufferDesc.Format, d->BufferCount, d->Windowed, (int)d->SwapEffect, d->Flags); }
    } else Log("[Overlay] CreateSwapChain hr=0x%08lx\n", hr);
    return hr;
}

HRESULT WINAPI h_CreateDXGIFactory1(REFIID riid, void** pp) {
    const HRESULT hr = o_CreateFactory1(riid, pp);
    if (SUCCEEDED(hr) && pp && *pp && !o_CreateSwapChain) {
        void** vt = *(void***)*pp;
        const bool ok = PatchSlot(vt, 10, (void*)&h_CreateSwapChain, (void**)&o_CreateSwapChain);   // IDXGIFactory::CreateSwapChain
        Log("[Overlay] CreateDXGIFactory1 -> factory %p vtable %p CreateSwapChain wrapped=%d\n", *pp, (void*)vt, ok);
    }
    return hr;
}

// ---- D3D11 state backup (the RmlUi renderer restores most state but not render targets / PS constant buffers) ------
struct D3DState {
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]; ID3D11DepthStencilView* dsv;
    UINT nVp, nSc; D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]; D3D11_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    ID3D11RasterizerState* rs; ID3D11BlendState* bs; FLOAT bf[4]; UINT mask; ID3D11DepthStencilState* ds; UINT sref;
    ID3D11ShaderResourceView* psSrv[2]; ID3D11SamplerState* psSmp[2]; ID3D11Buffer* psCb[2]; ID3D11Buffer* vsCb[2];
    ID3D11PixelShader* ps; ID3D11VertexShader* vs; ID3D11GeometryShader* gs;
    D3D11_PRIMITIVE_TOPOLOGY topo; ID3D11Buffer* ib; DXGI_FORMAT ibFmt; UINT ibOff; ID3D11Buffer* vb; UINT vbStride, vbOff; ID3D11InputLayout* il;
};
void Backup(ID3D11DeviceContext* c, D3DState& s) {
    memset(&s, 0, sizeof(s));
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtv, &s.dsv);
    s.nVp = s.nSc = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    c->RSGetViewports(&s.nVp, s.vp); c->RSGetScissorRects(&s.nSc, s.sc); c->RSGetState(&s.rs);
    c->OMGetBlendState(&s.bs, s.bf, &s.mask); c->OMGetDepthStencilState(&s.ds, &s.sref);
    c->PSGetShaderResources(0, 2, s.psSrv); c->PSGetSamplers(0, 2, s.psSmp); c->PSGetConstantBuffers(0, 2, s.psCb); c->VSGetConstantBuffers(0, 2, s.vsCb);
    c->PSGetShader(&s.ps, nullptr, nullptr); c->VSGetShader(&s.vs, nullptr, nullptr); c->GSGetShader(&s.gs, nullptr, nullptr);
    c->IAGetPrimitiveTopology(&s.topo); c->IAGetIndexBuffer(&s.ib, &s.ibFmt, &s.ibOff); c->IAGetVertexBuffers(0, 1, &s.vb, &s.vbStride, &s.vbOff); c->IAGetInputLayout(&s.il);
}
template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
void Restore(ID3D11DeviceContext* c, D3DState& s) {
    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtv, s.dsv);
    c->RSSetViewports(s.nVp, s.vp); c->RSSetScissorRects(s.nSc, s.sc); c->RSSetState(s.rs);
    c->OMSetBlendState(s.bs, s.bf, s.mask); c->OMSetDepthStencilState(s.ds, s.sref);
    c->PSSetShaderResources(0, 2, s.psSrv); c->PSSetSamplers(0, 2, s.psSmp); c->PSSetConstantBuffers(0, 2, s.psCb); c->VSSetConstantBuffers(0, 2, s.vsCb);
    c->PSSetShader(s.ps, nullptr, 0); c->VSSetShader(s.vs, nullptr, 0); c->GSSetShader(s.gs, nullptr, 0);
    c->IASetPrimitiveTopology(s.topo); c->IASetIndexBuffer(s.ib, s.ibFmt, s.ibOff); c->IASetVertexBuffers(0, 1, &s.vb, &s.vbStride, &s.vbOff); c->IASetInputLayout(s.il);
    for (auto& p : s.rtv) Rel(p);
    Rel(s.dsv); Rel(s.rs); Rel(s.bs); Rel(s.ds); for (auto& p : s.psSrv) Rel(p); for (auto& p : s.psSmp) Rel(p);
    for (auto& p : s.psCb) Rel(p); for (auto& p : s.vsCb) Rel(p); Rel(s.ps); Rel(s.vs); Rel(s.gs); Rel(s.ib); Rel(s.vb); Rel(s.il);
}

// ---- panel actions (S2: copy only; host/join/stop arrive with the bridge-on-demand slice) ---------------------------
bool CopyText(const char* text) {
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    const size_t n = strlen(text) + 1; bool ok = false;
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n)) {
        if (void* p = GlobalLock(g)) { memcpy(p, text, n); GlobalUnlock(g); ok = SetClipboardData(CF_TEXT, g) != nullptr; }
        if (!ok) GlobalFree(g);
    }
    CloseClipboard();
    return ok;
}
// Panel buttons arrive on the render thread; they are queued and run on the overlay thread (starting or stopping the
// bridge can take up to ~2 s, which must never stall a frame).
char g_pendAct[16] = {0}, g_pendArg[16] = {0}, g_note[96] = {0};
// The role is locked (Phase1_SetEntityHost) only once the bridge reports "connected", so a failed Join or a typo never
// forces a restart before the player can Host instead. Until then it is only pending (shown, not in force).
volatile LONG g_pendingRole = -1;
void OnAction(const char* a, const char* arg) {
    Log("[Overlay] action %s %s\n", a, arg ? arg : "");
    EnterCriticalSection(&g_cs);
    lstrcpynA(g_pendAct, a, sizeof(g_pendAct)); lstrcpynA(g_pendArg, arg ? arg : "", sizeof(g_pendArg));
    LeaveCriticalSection(&g_cs);
}
void RunPendingAction() {
    char a[16], arg[16];
    EnterCriticalSection(&g_cs);
    lstrcpynA(a, g_pendAct, sizeof(a)); lstrcpynA(arg, g_pendArg, sizeof(arg)); g_pendAct[0] = 0;
    LeaveCriticalSection(&g_cs);
    if (!a[0]) return;
    char err[128] = {0}, note[96] = {0};
    if (!strcmp(a, "copy")) {
        char code[16]; EnterCriticalSection(&g_cs); lstrcpynA(code, g_status.code, sizeof(code)); LeaveCriticalSection(&g_cs);
        const bool ok = code[0] && CopyText(code);
        _snprintf(note, sizeof(note), ok ? "Code %s copied" : "Could not copy the code", code);
    } else if (!strcmp(a, "host")) {
        if (Phase1_EntityHostState() == 0) lstrcpynA(note, "This game joined a friend - restart it from the launcher to host", sizeof(note));
        else if (Net::EosHost(err, sizeof(err))) { g_pendingRole = 1; lstrcpynA(note, "Starting... the code appears in a few seconds", sizeof(note)); }
        else _snprintf(note, sizeof(note), "Host failed: %s", err);
    } else if (!strcmp(a, "join")) {
        char c[16] = {0}; int n = 0;
        for (const char* q = arg; *q && n < 15; ++q) if (isalnum((unsigned char)*q)) c[n++] = *q;
        if (n != 6) lstrcpynA(note, "A lobby code is 6 letters/digits", sizeof(note));
        else if (Phase1_EntityHostState() == 1) lstrcpynA(note, "This game is hosting - restart it from the launcher to join", sizeof(note));
        else if (Net::EosJoin(arg, err, sizeof(err))) { g_pendingRole = 0; _snprintf(note, sizeof(note), "Joining %s...", arg); }
        else _snprintf(note, sizeof(note), "Join failed: %s", err);
    } else if (!strcmp(a, "stop")) {
        Net::EosStop(); if (Phase1_EntityHostState() < 0) g_pendingRole = -1; lstrcpynA(note, "Stopped", sizeof(note));
    }
    note[sizeof(note) - 1] = 0;
    Log("[Overlay] %s -> %s\n", a, note);
    EnterCriticalSection(&g_cs); lstrcpynA(g_note, note, sizeof(g_note)); LeaveCriticalSection(&g_cs);
}

// ---- window messages: queued on the window thread, handed to the panel on the render thread -------------------------
bool IsInputMsg(UINT m) {
    switch (m) {
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN:
        case WM_MBUTTONUP: case WM_MOUSEWHEEL: case WM_LBUTTONDBLCLK: case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
        case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_INPUT: return true;
    }
    return false;
}
LRESULT CALLBACK OverlayWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEMOVE) InterlockedIncrement(&c_wmMouse);
    if (m == WM_KEYDOWN || m == WM_CHAR) InterlockedIncrement(&c_wmKey);
    if (g_open && IsInputMsg(m)) {
        if (m == WM_KEYDOWN && (int)w == g_key) return 0;   // the toggle key itself (handled in Present)
        EnterCriticalSection(&g_cs);
        if (m == WM_MOUSEMOVE) g_sawWmMouse = true;
        if (m != WM_INPUT && g_qn < (int)(sizeof(g_q) / sizeof(g_q[0]))) g_q[g_qn++] = QMsg{ m, w, l };
        LeaveCriticalSection(&g_cs);
        if (m == WM_INPUT) return DefWindowProcA(h, m, w, l);   // WM_INPUT must reach DefWindowProc for cleanup
        return 0;
    }
    return CallWindowProcA(g_origWndProc, h, m, w, l);
}

// ---- per-frame work on the game's render thread ----------------------------------------------------------------------
int g_cx = 0, g_cy = 0, g_w = 0, g_h = 0;   // software cursor from DirectInput deltas (when no WM_MOUSEMOVE arrives)
bool g_keyWas = false;
void SetOpen(bool v) {
    if (v == g_open) return;
    EnterCriticalSection(&g_cs);
    g_open = v; g_qn = 0; g_diDx = g_diDy = 0; memset(g_diBtn, 0, sizeof(g_diBtn)); g_sawWmMouse = false;
    LeaveCriticalSection(&g_cs);
    g_cx = g_w / 2; g_cy = g_h / 2;
    OverlayUi::SetVisible(v);
    Log("[Overlay] panel %s\n", v ? "open" : "closed");
}

void OnPresent(IDXGISwapChain* sc) {
    InterlockedIncrement(&c_present);
    if (!g_enable || g_uiFailed) return;
    if (!g_uiReady) {
        DXGI_SWAP_CHAIN_DESC d{}; if (FAILED(sc->GetDesc(&d))) return;
        ID3D11Device* dev = nullptr;
        if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) { g_uiFailed = true; Log("[Overlay] GetDevice failed - overlay off\n"); return; }
        char dir[MAX_PATH]; _snprintf(dir, sizeof(dir), "%s\\coop_ui", Paths::ModuleDir()); dir[sizeof(dir) - 1] = 0;
        RECT r{}; GetClientRect(d.OutputWindow, &r);
        g_w = r.right - r.left; g_h = r.bottom - r.top; if (g_w < 1 || g_h < 1) { g_w = d.BufferDesc.Width; g_h = d.BufferDesc.Height; }
        const bool ok = OverlayUi::Init(dev, d.OutputWindow, (int)d.BufferDesc.Width, (int)d.BufferDesc.Height, dir, OnAction, LogLine);
        dev->Release();
        if (!ok) { g_uiFailed = true; Log("[Overlay] UI init failed - overlay off (game unaffected)\n"); return; }
        g_hwnd = d.OutputWindow;
        g_origWndProc = (WNDPROC)SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)&OverlayWndProc);
        Log("[Overlay] window %p subclassed=%d client=%dx%d backbuffer=%ux%u\n", (void*)g_hwnd, g_origWndProc ? 1 : 0, g_w, g_h, d.BufferDesc.Width, d.BufferDesc.Height);
        g_uiReady = true;
    }
    // toggle: polled key state (works whatever input API the game uses), only while the game window is in front
    const bool down = (GetAsyncKeyState(g_key) & 0x8000) != 0 && GetForegroundWindow() == g_hwnd;
    if (down && !g_keyWas) SetOpen(!g_open);
    g_keyWas = down;

    QMsg q[256]; int qn = 0; int dx = 0, dy = 0; BYTE btn[3]; bool btnSeen, sawWm; OverlayUi::Status st; bool stDirty;
    EnterCriticalSection(&g_cs);
    qn = g_qn; memcpy(q, g_q, qn * sizeof(QMsg)); g_qn = 0;
    dx = g_diDx; dy = g_diDy; g_diDx = g_diDy = 0; memcpy(btn, g_diBtn, 3); btnSeen = g_diBtnSeen; sawWm = g_sawWmMouse;
    st = g_status; stDirty = g_statusDirty; g_statusDirty = false;
    LeaveCriticalSection(&g_cs);
    if (stDirty) OverlayUi::SetStatus(st);
    if (!g_open) return;

    // Map client coordinates to the back buffer (they differ when the window is resized without ResizeBuffers).
    DXGI_SWAP_CHAIN_DESC d{}; sc->GetDesc(&d);
    RECT r{}; GetClientRect(g_hwnd, &r);
    const int cw = r.right - r.left > 0 ? r.right - r.left : 1, ch = r.bottom - r.top > 0 ? r.bottom - r.top : 1;
    g_w = cw; g_h = ch;
    auto scale = [&](LPARAM l) -> LPARAM {
        const int x = (short)LOWORD(l) * (int)d.BufferDesc.Width / cw, y = (short)HIWORD(l) * (int)d.BufferDesc.Height / ch;
        return MAKELPARAM(x, y);
    };
    for (int i = 0; i < qn; ++i) {
        LPARAM l = q[i].l;
        if (q[i].m == WM_MOUSEMOVE || (q[i].m >= WM_LBUTTONDOWN && q[i].m <= WM_MBUTTONDBLCLK)) l = scale(l);
        OverlayUi::OnMessage(g_hwnd, q[i].m, q[i].w, l);
    }
    if (!sawWm && (dx || dy || btnSeen)) {   // DirectInput-only mouse: drive a cursor from the deltas
        g_cx = max(0, min(cw - 1, g_cx + dx)); g_cy = max(0, min(ch - 1, g_cy + dy));
        static BYTE last[3] = {0};
        const LPARAM p = scale(MAKELPARAM(g_cx, g_cy));
        OverlayUi::OnMessage(g_hwnd, WM_MOUSEMOVE, 0, p);
        const UINT dn[3] = { WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN }, up[3] = { WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP };
        for (int b = 0; b < 3; ++b) if ((btn[b] & 0x80) != (last[b] & 0x80)) OverlayUi::OnMessage(g_hwnd, (btn[b] & 0x80) ? dn[b] : up[b], 0, p);
        memcpy(last, btn, 3);
    }

    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) return;
    dev->GetImmediateContext(&ctx);
    if (!g_rtv) {
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb) { dev->CreateRenderTargetView(bb, nullptr, &g_rtv); bb->Release(); }
        OverlayUi::Resize((int)d.BufferDesc.Width, (int)d.BufferDesc.Height);
    }
    if (g_rtv) {
        D3DState s; Backup(ctx, s);
        OverlayUi::Render(g_rtv);
        Restore(ctx, s);
    }
    ctx->Release(); dev->Release();
}

volatile LONG g_inPresent = 0;
int SehFilter(unsigned code) { Log("[Overlay] exception 0x%08x in the overlay frame - overlay disabled\n", code); return EXCEPTION_EXECUTE_HANDLER; }
void SafeOnPresent(IDXGISwapChain* sc) {
    __try { OnPresent(sc); }
    __except (SehFilter(GetExceptionCode())) { g_uiFailed = true; g_open = false; }
}
HRESULT STDMETHODCALLTYPE h_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (sc == g_sc && InterlockedIncrement(&g_inPresent) == 1) SafeOnPresent(sc);
    if (sc == g_sc) InterlockedDecrement(&g_inPresent);
    return o_Present(sc, sync, flags);
}
HRESULT STDMETHODCALLTYPE h_ResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    InterlockedIncrement(&c_resize);
    if (sc == g_sc && g_rtv) { g_rtv->Release(); g_rtv = nullptr; }   // our view must go before the buffers can resize
    const HRESULT hr = o_ResizeBuffers(sc, n, w, h, f, fl);
    Log("[Overlay] ResizeBuffers %ux%u fmt=%d hr=0x%08lx\n", w, h, (int)f, hr);
    return hr;
}

// ---- DirectInput: zero the game's keyboard/mouse reads while the panel is open ------------------------------------------
struct DiOrig { void* state; void* data; };
DiOrig g_diA{}, g_diW{};
struct DevType { void* dev; BYTE type; } g_devs[16]; int g_ndev = 0;
BYTE DeviceType(IDirectInputDevice8A* d) {   // DI8DEVTYPE_MOUSE 0x12, _KEYBOARD 0x13 (low byte of dwDevType)
    for (int i = 0; i < g_ndev; ++i) if (g_devs[i].dev == d) return g_devs[i].type;
    DIDEVICEINSTANCEA di{}; di.dwSize = sizeof(di);
    BYTE t = SUCCEEDED(d->GetDeviceInfo(&di)) ? (BYTE)(di.dwDevType & 0xFF) : 0;
    if (g_ndev < 16) g_devs[g_ndev++] = DevType{ d, t };
    return t;
}
template <DiOrig* O>
struct DiHooks {
    static HRESULT STDMETHODCALLTYPE GetState(IDirectInputDevice8A* d, DWORD n, LPVOID p) {
        InterlockedIncrement(&c_diState);
        const HRESULT hr = ((HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A*, DWORD, LPVOID))O->state)(d, n, p);
        if (g_open && SUCCEEDED(hr) && p) {
            if (DeviceType(d) == DI8DEVTYPE_MOUSE && n >= sizeof(DIMOUSESTATE)) {
                const DIMOUSESTATE* ms = (const DIMOUSESTATE*)p;
                EnterCriticalSection(&g_cs); g_diDx += ms->lX; g_diDy += ms->lY; memcpy(g_diBtn, ms->rgbButtons, 3); g_diBtnSeen = true; LeaveCriticalSection(&g_cs);
            }
            memset(p, 0, n); InterlockedIncrement(&c_diZeroed);
        }
        return hr;
    }
    static HRESULT STDMETHODCALLTYPE GetData(IDirectInputDevice8A* d, DWORD cb, LPDIDEVICEOBJECTDATA od, LPDWORD n, DWORD f) {
        InterlockedIncrement(&c_diData);
        const HRESULT hr = ((HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD))O->data)(d, cb, od, n, f);
        if (g_open && SUCCEEDED(hr) && n && *n && od && !(f & DIGDD_PEEK)) {
            if (DeviceType(d) == DI8DEVTYPE_MOUSE && cb >= sizeof(DIDEVICEOBJECTDATA)) {
                EnterCriticalSection(&g_cs);
                for (DWORD i = 0; i < *n; ++i) {
                    const DIDEVICEOBJECTDATA* e = (const DIDEVICEOBJECTDATA*)((const BYTE*)od + i * cb);
                    if (e->dwOfs == DIMOFS_X) g_diDx += (int)e->dwData; else if (e->dwOfs == DIMOFS_Y) g_diDy += (int)e->dwData;
                    else if (e->dwOfs >= DIMOFS_BUTTON0 && e->dwOfs <= DIMOFS_BUTTON2) { g_diBtn[e->dwOfs - DIMOFS_BUTTON0] = (BYTE)e->dwData; g_diBtnSeen = true; }
                }
                LeaveCriticalSection(&g_cs);
            }
            *n = 0; InterlockedIncrement(&c_diZeroed);
        }
        return hr;
    }
};
typedef HRESULT(WINAPI* DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
FARPROC SysProc(const char* dll, const char* fn) {   // System32 copy, never the game folder's dinput8 proxy
    char p[MAX_PATH]; GetSystemDirectoryA(p, MAX_PATH); lstrcatA(p, "\\"); lstrcatA(p, dll);
    HMODULE m = LoadLibraryA(p);
    return m ? GetProcAddress(m, fn) : nullptr;
}
template <DiOrig* O>
void HookDi(void** vt, const char* tag) {
    const bool a = PatchSlot(vt, 9, (void*)&DiHooks<O>::GetState, &O->state);    // IDirectInputDevice8::GetDeviceState
    const bool b = PatchSlot(vt, 10, (void*)&DiHooks<O>::GetData, &O->data);     // IDirectInputDevice8::GetDeviceData
    Log("[Overlay] dinput %s device vtable %p patched state=%d data=%d\n", tag, (void*)vt, a, b);
}
void HookDInput() {
    auto create = (DirectInput8Create_t)SysProc("dinput8.dll", "DirectInput8Create");
    if (!create) { Log("[Overlay] DirectInput8Create not found\n"); return; }
    HMODULE self = GetModuleHandleA(nullptr);
    IDirectInput8A* a = nullptr; IDirectInput8W* w = nullptr;
    if (SUCCEEDED(create(self, DIRECTINPUT_VERSION, IID_IDirectInput8A, (void**)&a, nullptr))) {
        IDirectInputDevice8A* k = nullptr;
        if (SUCCEEDED(a->CreateDevice(GUID_SysKeyboard, &k, nullptr))) { HookDi<&g_diA>(*(void***)k, "A"); k->Release(); }
        a->Release();
    }
    if (SUCCEEDED(create(self, DIRECTINPUT_VERSION, IID_IDirectInput8W, (void**)&w, nullptr))) {
        IDirectInputDevice8W* k = nullptr;
        if (SUCCEEDED(w->CreateDevice(GUID_SysKeyboard, &k, nullptr))) {
            void** vt = *(void***)k;
            if (g_diA.state && vt[9] == (void*)&DiHooks<&g_diA>::GetState) Log("[Overlay] dinput W shares the A vtable\n");
            else HookDi<&g_diW>(vt, "W");
            k->Release();
        }
        w->Release();
    }
}

// ---- status (S2: role from the ini + the bridge's status file) and the 5 s probe line -------------------------------
void ReadStatus() {
    OverlayUi::Status s{};
    char role[16] = {0}, transport[16] = {0};
    GetPrivateProfileStringA("Net", "Transport", "udp", transport, sizeof(transport), Paths::Ini());
    GetPrivateProfileStringA("Net", "EosRole", "", role, sizeof(role), Paths::Ini());
    if (!_stricmp(transport, "eos")) {
        if (GetPrivateProfileIntA("Overlay", "RoleFromPanel", 0, Paths::Ini())) {   // role picked in this panel
            const int r = Phase1_EntityHostState() >= 0 ? Phase1_EntityHostState() : (int)g_pendingRole;
            lstrcpynA(s.role, r == 1 ? "host" : r == 0 ? "join" : "any", sizeof(s.role));
        } else lstrcpynA(s.role, role, sizeof(s.role));
    }
    lstrcpynA(s.state, "off", sizeof(s.state));
    char path[MAX_PATH], line[512] = {0};
    _snprintf(path, sizeof(path), "%s\\eos\\status.txt", Paths::ModuleDir()); path[sizeof(path) - 1] = 0;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") == 0 && f) { if (!fgets(line, sizeof(line), f)) line[0] = 0; fclose(f); }
    auto field = [&](const char* key, char* dst, size_t cap) {
        const char* p = strstr(line, key); if (!p) return;
        p += strlen(key); size_t i = 0; while (*p && *p != ' ' && *p != '\r' && *p != '\n' && i + 1 < cap) dst[i++] = *p++; dst[i] = 0;
    };
    field("state=", s.state, sizeof(s.state)); field("code=", s.code, sizeof(s.code)); field("link=", s.link, sizeof(s.link));
    if (!strcmp(s.code, "-")) s.code[0] = 0;
    if (!strcmp(s.link, "none") || !strcmp(s.link, "-")) s.link[0] = 0;
    if (!Net::EosRunning() && strcmp(s.state, "off") && strncmp(s.state, "failed", 6)) {   // stale file from an earlier run
        lstrcpynA(s.state, "stopped", sizeof(s.state)); s.code[0] = 0; s.link[0] = 0;
    }
    if (!strcmp(s.state, "connected") && g_pendingRole >= 0 && Phase1_EntityHostState() < 0
        && GetPrivateProfileIntA("Overlay", "RoleFromPanel", 0, Paths::Ini())) {
        const bool ok = Phase1_SetEntityHost((int)g_pendingRole);
        Log("[Overlay] connected: role %s locked=%d\n", g_pendingRole ? "host" : "guest", ok);
    }
    if (!Net::Enabled()) lstrcpynA(s.peer, "load into the world to link", sizeof(s.peer));
    else {
        const uint32_t ms = Net::MillisSinceRecv();
        if (ms == 0xFFFFFFFFu) lstrcpynA(s.peer, "waiting for the other game", sizeof(s.peer));
        else if (ms < 3000) lstrcpynA(s.peer, "linked", sizeof(s.peer));
        else _snprintf(s.peer, sizeof(s.peer), "no data for %u s", ms / 1000);
    }
    EnterCriticalSection(&g_cs);
    lstrcpynA(s.note, g_note, sizeof(s.note));
    if (memcmp(&s, &g_status, sizeof(s)) != 0) { g_status = s; g_statusDirty = true; }
    LeaveCriticalSection(&g_cs);
}

unsigned __stdcall OverlayThread(void*) {
    HookDInput();
    LONG lastP = 0;
    for (int tick = 0;; ++tick) {
        Sleep(250);
        if (g_enable) { RunPendingAction(); ReadStatus(); }
        if (tick % 20 == 19) {
            const LONG p = c_present;
            Log("[Overlay] t=%ds present=%ld (%ld/s) swapchains=%ld resize=%ld | wm mouse=%ld key=%ld | di state=%ld data=%ld zeroed=%ld | open=%d ui=%d%s\n",
                (tick + 1) / 4, p, (p - lastP) / 5, c_scCreated, c_resize, c_wmMouse, c_wmKey, c_diState, c_diData, c_diZeroed,
                g_open ? 1 : 0, g_uiReady ? 1 : 0, g_uiFailed ? " FAILED" : "");
            lastP = p;
        }
    }
}

}  // namespace

namespace Overlay {

void InstallEarly() {
    g_enable = GetPrivateProfileIntA("Overlay", "Enable", 0, Paths::Ini()) != 0;
    g_probe = g_enable || GetPrivateProfileIntA("Overlay", "Probe", 0, Paths::Ini()) != 0;
    if (!g_probe) return;
    g_key = GetPrivateProfileIntA("Overlay", "Key", VK_F10, Paths::Ini());
    InitializeCriticalSection(&g_logCs); InitializeCriticalSection(&g_cs); g_logInit = true;
    HMODULE dx = GetModuleHandleA("dxgi.dll");
    void* real = dx ? (void*)GetProcAddress(dx, "CreateDXGIFactory1") : nullptr;
    void* cur = nullptr;
    __try { cur = *kIatCreateDXGIFactory1; } __except (EXCEPTION_EXECUTE_HANDLER) { cur = nullptr; }
    Log("[Overlay] ---- pid=%lu enable=%d key=0x%x ui=%s | IAT 0x01E3A618 holds %p, dxgi!CreateDXGIFactory1=%p\n",
        GetCurrentProcessId(), g_enable, g_key, OverlayUi::Built() ? "rmlui" : "none", cur, real);
    if (!real || cur != real) { Log("[Overlay] IAT slot does not hold dxgi!CreateDXGIFactory1 - not patched (overlay off)\n"); return; }
    o_CreateFactory1 = (CreateDXGIFactory1_t)real;
    void* orig = nullptr;
    const bool ok = PatchSlot(kIatCreateDXGIFactory1, 0, (void*)&h_CreateDXGIFactory1, &orig);
    Log("[Overlay] IAT CreateDXGIFactory1 patched=%d\n", ok);
}

void Start() {
    if (!g_probe) return;
    _beginthreadex(nullptr, 0, OverlayThread, nullptr, 0, nullptr);
}

}  // namespace Overlay
