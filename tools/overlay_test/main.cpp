// overlay_test - x86 oracle for the RmlUi overlay (src/overlay_ui.*), run before anything touches the game.
// Same code path as in the game except the Present hook: our own window + D3D11 swap chain stand in for the game's.
//   overlay_test <coop_ui dir> [out.bmp] [--bgra]
// Checks (each prints PASS/FAIL; exit 0 only if all pass):
//   1 negative control: overlay hidden -> the back buffer is exactly the clear colour
//   2 visible host panel -> >2% of pixels differ from the clear colour, and the title gold (#d9b56a) is present
//   3 the D3D state the game had bound survives a frame (render target, viewport, blend state)
//   4 input: click the code box, type "ab12c9", click Join -> onAction("join", "AB12C9")
//   5 status -> host "Copy code" button appears only once a code is set
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "../../src/overlay_ui.h"
#pragma comment(lib, "d3d11.lib")

static int g_fail = 0;
static void Check(bool ok, const char* what) { printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++g_fail; }
static void Log(const char* s) { fputs(s, stdout); }
static std::string g_lastAction, g_lastArg;
static void OnAction(const char* a, const char* arg) { g_lastAction = a; g_lastArg = arg ? arg : ""; printf("  action %s %s\n", a, arg ? arg : ""); }

static const int W = 1280, H = 720;
static ID3D11Device* dev; static ID3D11DeviceContext* ctx; static IDXGISwapChain* sc; static ID3D11RenderTargetView* rtv;
static const float kClear[4] = { 0.10f, 0.20f, 0.30f, 1.0f };

struct Shot { int differ = 0, gold = 0; std::string bmp; };
static Shot Grab(const char* bmpPath, bool bgra) {
    Shot r;
    ID3D11Texture2D* bb = nullptr; sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr; dev->CreateTexture2D(&d, nullptr, &st);
    ctx->CopyResource(st, bb);
    D3D11_MAPPED_SUBRESOURCE m; ctx->Map(st, 0, D3D11_MAP_READ, 0, &m);
    const int cr = (int)(kClear[0] * 255 + 0.5f), cg = (int)(kClear[1] * 255 + 0.5f), cb = (int)(kClear[2] * 255 + 0.5f);
    FILE* f = bmpPath ? fopen(bmpPath, "wb") : nullptr;
    if (f) {
        BITMAPFILEHEADER fh = {}; BITMAPINFOHEADER ih = {};
        fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + W * H * 4;
        ih.biSize = sizeof(ih); ih.biWidth = W; ih.biHeight = -H; ih.biPlanes = 1; ih.biBitCount = 32;
        fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
    }
    for (int y = 0; y < H; ++y) {
        const unsigned char* row = (const unsigned char*)m.pData + y * m.RowPitch;
        for (int x = 0; x < W; ++x) {
            const unsigned char* p = row + x * 4;
            const int R = bgra ? p[2] : p[0], G = p[1], B = bgra ? p[0] : p[2];
            if (abs(R - cr) > 2 || abs(G - cg) > 2 || abs(B - cb) > 2) ++r.differ;
            if (abs(R - 0xd9) < 24 && abs(G - 0xb5) < 24 && abs(B - 0x6a) < 24) ++r.gold;
            if (f) { const unsigned char px[4] = { (unsigned char)B, (unsigned char)G, (unsigned char)R, 255 }; fwrite(px, 4, 1, f); }
        }
    }
    if (f) fclose(f);
    ctx->Unmap(st, 0); st->Release(); bb->Release();
    return r;
}

static void Frame(int n = 3) {
    for (int i = 0; i < n; ++i) {
        MSG msg; while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, kClear);
        OverlayUi::Render(rtv);
        sc->Present(0, 0);
    }
}

static HWND g_wnd;
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (OverlayUi::OnMessage(h, m, w, l)) return 0;
    return DefWindowProcA(h, m, w, l);
}
static void Click(int x, int y) {
    const LPARAM p = MAKELPARAM(x, y);
    SendMessageA(g_wnd, WM_MOUSEMOVE, 0, p);   // no pump before the click: TrackMouseEvent posts WM_MOUSELEAVE (real cursor is elsewhere)
    SendMessageA(g_wnd, WM_LBUTTONDOWN, MK_LBUTTON, p); SendMessageA(g_wnd, WM_LBUTTONUP, 0, p); Frame(1);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) { printf("usage: overlay_test <coop_ui dir> [out.bmp] [--bgra]\n"); return 2; }
    const char* uiDir = argv[1]; const char* bmp = (argc > 2 && argv[2][0] != '-') ? argv[2] : nullptr;
    bool bgra = false; for (int i = 2; i < argc; ++i) if (!strcmp(argv[i], "--bgra")) bgra = true;
    char full[MAX_PATH]; GetFullPathNameA(uiDir, MAX_PATH, full, nullptr);

    WNDCLASSA wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "OverlayTest";
    RegisterClassA(&wc);
    RECT rc = { 0, 0, W, H }; AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_wnd = CreateWindowA(wc.lpszClassName, "overlay_test", WS_OVERLAPPEDWINDOW, 50, 50, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(g_wnd, SW_SHOWNOACTIVATE);

    DXGI_SWAP_CHAIN_DESC sd = {}; sd.BufferCount = 2; sd.BufferDesc.Width = W; sd.BufferDesc.Height = H;
    sd.BufferDesc.Format = bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = g_wnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx))) {
        printf("FAIL  D3D11 device\n"); return 1;
    }
    ID3D11Texture2D* bb = nullptr; sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb); dev->CreateRenderTargetView(bb, nullptr, &rtv); bb->Release();
    printf("x86 build, sizeof(void*)=%d, format=%s\n", (int)sizeof(void*), bgra ? "B8G8R8A8" : "R8G8B8A8");

    if (!OverlayUi::Init(dev, g_wnd, W, H, full, OnAction, Log)) { printf("FAIL  OverlayUi::Init\n"); return 1; }

    // 1 negative control
    OverlayUi::Status s = {}; strcpy(s.role, "host"); strcpy(s.state, "off");
    OverlayUi::SetStatus(s);
    Frame(); Shot a = Grab(nullptr, bgra);
    Check(a.differ == 0, "hidden overlay leaves the frame untouched");

    // 5 (part 1) no code yet -> no Copy button
    OverlayUi::SetVisible(true); Frame();
    int x, y;
    Check(!OverlayUi::ElementCenter("btn-copy", &x, &y) && OverlayUi::ElementCenter("btn-host", &x, &y), "host panel, no code: Host shown, Copy hidden");

    // 2 visible host panel with a code
    strcpy(s.state, "hosting"); strcpy(s.code, "YV76JD"); strcpy(s.link, "RELAYED"); strcpy(s.peer, "peer up 410 ms");
    OverlayUi::SetStatus(s); Frame();
    Shot b = Grab(bmp, bgra);
    printf("  differ=%d (%.1f%%) gold=%d\n", b.differ, 100.0 * b.differ / (W * H), b.gold);
    Check(b.differ > W * H / 50 && b.gold > 200, "visible panel draws (pixels changed, title colour present)");
    Check(OverlayUi::ElementCenter("btn-copy", &x, &y) && !OverlayUi::ElementCenter("btn-host", &x, &y), "host panel, code set: Copy shown, Host hidden");
    if (OverlayUi::ElementCenter("btn-copy", &x, &y)) { Click(x, y); Check(g_lastAction == "copy", "click Copy -> action copy"); }

    // 3 caller state survives
    D3D11_VIEWPORT vp = { 7, 9, 100, 50, 0, 1 }; ctx->RSSetViewports(1, &vp);
    D3D11_BLEND_DESC bd = {}; bd.RenderTarget[0].RenderTargetWriteMask = 0x0F; ID3D11BlendState* bs = nullptr; dev->CreateBlendState(&bd, &bs);
    const float bf[4] = { 0.25f, 0.5f, 0.75f, 1 }; ctx->OMSetBlendState(bs, bf, 0xFFFFFFFF);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    OverlayUi::Render(rtv);
    UINT n = 1; D3D11_VIEWPORT vp2 = {}; ctx->RSGetViewports(&n, &vp2);
    ID3D11BlendState* bs2 = nullptr; float bf2[4]; UINT mask = 0; ctx->OMGetBlendState(&bs2, bf2, &mask);
    ID3D11RenderTargetView* rtv2 = nullptr; ctx->OMGetRenderTargets(1, &rtv2, nullptr);
    Check(vp2.TopLeftX == 7 && vp2.Width == 100, "viewport restored by the renderer");
    Check(bs2 == bs, "blend state restored by the renderer");
    printf("  render target after frame: %s (the hook layer must restore it; the renderer does not back it up)\n", rtv2 == rtv ? "same" : "CHANGED");
    if (bs2) bs2->Release(); if (rtv2) rtv2->Release(); bs->Release();
    ctx->RSSetViewports(1, &vp);
    D3D11_VIEWPORT full_vp = { 0, 0, (float)W, (float)H, 0, 1 }; ctx->RSSetViewports(1, &full_vp);

    // 4 join flow through window messages
    strcpy(s.role, "join"); strcpy(s.state, "off"); strcpy(s.code, ""); strcpy(s.link, ""); strcpy(s.peer, "");
    OverlayUi::SetStatus(s); Frame();
    if (OverlayUi::ElementCenter("codebox", &x, &y)) {
        Click(x, y);
        for (const char* c = "ab12c9"; *c; ++c) { SendMessageA(g_wnd, WM_CHAR, (WPARAM)*c, 0); Frame(1); }
        if (OverlayUi::ElementCenter("btn-join", &x, &y)) Click(x, y);
        Check(g_lastAction == "join" && g_lastArg == "AB12C9", "type code + click Join -> action join AB12C9");
    } else Check(false, "code box present on the join panel");
    Check(OverlayUi::OnMessage(g_wnd, WM_KEYDOWN, 'W', 0), "keys are consumed while open");
    OverlayUi::SetVisible(false);
    Check(!OverlayUi::OnMessage(g_wnd, WM_KEYDOWN, 'W', 0), "keys pass through when closed");

    OverlayUi::Shutdown();
    rtv->Release(); sc->Release(); ctx->Release(); dev->Release();
    printf("%s (%d failed)\n", g_fail ? "RESULT FAIL" : "RESULT PASS", g_fail);
    return g_fail ? 1 : 0;
}
