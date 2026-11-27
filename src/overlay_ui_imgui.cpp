// In-game co-op panel with Dear ImGui (see overlay_ui.h) - the default UI. The only ImGui-aware translation unit.
// Runs entirely on the game's render thread (called from the Present wrap in overlay.cpp): no engine calls, no file
// or ini I/O. Buttons only call the ActionFn, which overlay.cpp queues for its own thread.
// imgui_impl_win32 is NOT used: it reads GetCursorPos/GetClientRect and calls SetCapture/TrackMouseEvent, which
// assume the window thread and client == back-buffer size. overlay.cpp already hands us window messages in
// back-buffer coordinates (including the DirectInput-driven cursor), so they are fed to ImGui's io directly.
#define _CRT_SECURE_NO_WARNINGS
#include "overlay_ui.h"

#ifndef AC4COOP_OVERLAY
#define AC4COOP_OVERLAY 0
#endif
#ifndef AC4COOP_UI_RMLUI
#define AC4COOP_UI_RMLUI 0
#endif

#if AC4COOP_OVERLAY && !AC4COOP_UI_RMLUI
#include <d3d11.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cctype>
#include "imgui.h"
#include "backends/imgui_impl_dx11.h"

namespace {

OverlayUi::ActionFn g_onAction = nullptr;
OverlayUi::LogFn g_log = nullptr;
void Logf(const char* fmt, ...) {
    if (!g_log) return;
    char b[512]; va_list ap; va_start(ap, fmt); _vsnprintf(b, sizeof(b) - 1, fmt, ap); va_end(ap); b[sizeof(b) - 1] = 0;
    g_log(b);
}

ID3D11DeviceContext* g_ctx = nullptr;
bool g_ready = false, g_visible = false;
int g_w = 0, g_h = 0;
LARGE_INTEGER g_qpf{}, g_last{};
OverlayUi::Status g_st{};
char g_input[16] = {0};

// Element rectangles from the last drawn frame (ElementCenter, same ids as overlay.rml's buttons).
struct Rect { const char* id; float x0, y0, x1, y1; };
Rect g_rects[8]; int g_rectN = 0;
void Remember(const char* id) {
    if (g_rectN >= 8) return;
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    g_rects[g_rectN++] = Rect{ id, a.x, a.y, b.x, b.y };
}

const char* Or(const char* s) { return (s && *s) ? s : "-"; }

ImGuiKey MapVk(WPARAM vk) {
    switch (vk) {
        case VK_TAB: return ImGuiKey_Tab;           case VK_LEFT: return ImGuiKey_LeftArrow;
        case VK_RIGHT: return ImGuiKey_RightArrow;  case VK_UP: return ImGuiKey_UpArrow;
        case VK_DOWN: return ImGuiKey_DownArrow;    case VK_HOME: return ImGuiKey_Home;
        case VK_END: return ImGuiKey_End;           case VK_DELETE: return ImGuiKey_Delete;
        case VK_BACK: return ImGuiKey_Backspace;    case VK_RETURN: return ImGuiKey_Enter;
        case VK_ESCAPE: return ImGuiKey_Escape;     case VK_CONTROL: return ImGuiKey_LeftCtrl;
        case VK_SHIFT: return ImGuiKey_LeftShift;
        case 'A': return ImGuiKey_A; case 'C': return ImGuiKey_C; case 'V': return ImGuiKey_V; case 'X': return ImGuiKey_X;
    }
    return ImGuiKey_None;
}

void Panel() {
    const OverlayUi::Status& s = g_st;
    // Same derived flags as overlay_ui.cpp (RmlUi) Derive().
    const char* state = s.state[0] ? s.state : "off";
    const bool off = !strcmp(state, "off") || !strcmp(state, "stopped");
    const bool failed = !strncmp(state, "failed", 6);
    const bool any = !strcmp(s.role, "any");
    const bool canStart = off || failed, hasCode = s.code[0] != 0, active = !off && !failed;
    const bool noRole = strcmp(s.role, "host") && strcmp(s.role, "join") && !any;
    const bool showHost = !strcmp(s.role, "host") || any, showJoin = !strcmp(s.role, "join") || any;

    ImGui::SetNextWindowPos(ImVec2(g_w * 0.5f, g_h * 0.12f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::Begin("AC4 Co-op", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
    ImGui::Text("Role   %s", Or(s.role));
    if (!strcmp(state, "connected")) ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "State  %s", state);
    else if (failed) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "State  %s", state);
    else ImGui::Text("State  %s", state);
    ImGui::Text("Code   %s", Or(s.code));
    ImGui::Text("Link   %s", Or(s.link));
    ImGui::Text("Peer   %s", Or(s.peer));
    if (noRole) ImGui::TextDisabled("No co-op role: [Net] Transport=eos with EosRole, or [Overlay] RoleFromPanel=1");

    if (canStart && showHost) { if (ImGui::Button("Host")) g_onAction("host", ""); Remember("btn-host"); }
    if (canStart && showJoin) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
        ImGui::InputText("##code", g_input, sizeof(g_input), ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank);
        Remember("codebox");
        ImGui::SameLine();
        if (ImGui::Button("Join")) {
            char code[16]; lstrcpynA(code, g_input, sizeof(code));
            for (char* c = code; *c; ++c) *c = (char)toupper((unsigned char)*c);
            g_onAction("join", code);
        }
        Remember("btn-join");
    }
    if (hasCode) { if (ImGui::Button("Copy code")) g_onAction("copy", ""); Remember("btn-copy"); }
    if (active) { if (hasCode) ImGui::SameLine(); if (ImGui::Button("Stop")) g_onAction("stop", ""); Remember("btn-stop"); }
    if (s.note[0]) ImGui::TextWrapped("%s", s.note);

    ImGui::Separator();   // read-only status rows
    ImGui::TextDisabled("proto %u | build %s | asi %s", s.proto, Or(s.commit), Or(s.asi));
    ImGui::TextDisabled("puppet %s | ship binds %d | char binds/5s %d", s.puppet ? "yes" : "no", s.shipsBound, s.guardsBound);
    ImGui::End();
}

}  // namespace

namespace OverlayUi {

bool Built() { return true; }
const char* Name() { return "imgui"; }

bool Init(ID3D11Device* dev, HWND, int w, int h, const char*, ActionFn onAction, LogFn log) {
    g_onAction = onAction; g_log = log;
    if (g_ready) return true;
    if (!dev || w < 1 || h < 1) { Logf("[Overlay] imgui: bad init args %dx%d\n", w, h); return false; }
    dev->GetImmediateContext(&g_ctx);
    if (!g_ctx) { Logf("[Overlay] imgui: no immediate context\n"); return false; }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr;   // no file I/O from the render thread
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.MouseDrawCursor = true;                             // the game hides the OS cursor; ImGui draws one
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.FontGlobalScale = h >= 1400 ? 2.0f : h >= 900 ? 1.5f : 1.0f;
    ImGui::StyleColorsDark();
    if (!ImGui_ImplDX11_Init(dev, g_ctx)) { Logf("[Overlay] imgui: ImGui_ImplDX11_Init failed\n"); ImGui::DestroyContext(); g_ctx->Release(); g_ctx = nullptr; return false; }
    QueryPerformanceFrequency(&g_qpf); QueryPerformanceCounter(&g_last);
    g_w = w; g_h = h; g_ready = true;
    Logf("[Overlay] ImGui %s ready %dx%d\n", IMGUI_VERSION, w, h);
    return true;
}

void Resize(int w, int h) {
    if (!g_ready || w < 1 || h < 1) return;
    g_w = w; g_h = h;
    ImGui::GetIO().DisplaySize = ImVec2((float)w, (float)h);
}

void Render(ID3D11RenderTargetView* rtv) {
    if (!g_ready || !g_visible || !rtv) return;
    ImGuiIO& io = ImGui::GetIO();
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    float dt = (float)(now.QuadPart - g_last.QuadPart) / (float)g_qpf.QuadPart; g_last = now;
    io.DeltaTime = dt > 0.0f && dt < 1.0f ? dt : 1.0f / 60.0f;
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    g_rectN = 0;
    Panel();
    ImGui::Render();
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

bool OnMessage(HWND, UINT msg, WPARAM wp, LPARAM lp) {
    if (!g_ready || !g_visible) return false;
    ImGuiIO& io = ImGui::GetIO();
    switch (msg) {
        case WM_MOUSEMOVE: io.AddMousePosEvent((float)(short)LOWORD(lp), (float)(short)HIWORD(lp)); return true;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: io.AddMouseButtonEvent(0, true); return true;
        case WM_LBUTTONUP: io.AddMouseButtonEvent(0, false); return true;
        case WM_RBUTTONDOWN: io.AddMouseButtonEvent(1, true); return true;
        case WM_RBUTTONUP: io.AddMouseButtonEvent(1, false); return true;
        case WM_MBUTTONDOWN: io.AddMouseButtonEvent(2, true); return true;
        case WM_MBUTTONUP: io.AddMouseButtonEvent(2, false); return true;
        case WM_MOUSEWHEEL: io.AddMouseWheelEvent(0.0f, (float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA); return true;
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: {
            const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
            if (wp == VK_CONTROL) io.AddKeyEvent(ImGuiMod_Ctrl, down);
            if (wp == VK_SHIFT) io.AddKeyEvent(ImGuiMod_Shift, down);
            const ImGuiKey k = MapVk(wp);
            if (k != ImGuiKey_None) io.AddKeyEvent(k, down);
            return true;
        }
        case WM_CHAR: if (wp >= 32 && wp < 0x10000) io.AddInputCharacter((unsigned)wp); return true;
        case WM_INPUT: case WM_IME_CHAR: case WM_IME_COMPOSITION: case WM_IME_STARTCOMPOSITION: case WM_IME_ENDCOMPOSITION:
            return true;   // gameplay input while the panel is open
    }
    return false;
}

void SetVisible(bool v) {
    if (!g_ready || v == g_visible) return;
    g_visible = v;
    ImGui::GetIO().AddFocusEvent(v);   // false releases every held key/button
    if (v) QueryPerformanceCounter(&g_last);
}
bool Visible() { return g_visible; }

void SetStatus(const Status& s) { g_st = s; }

bool ElementCenter(const char* id, int* x, int* y) {
    for (int i = 0; i < g_rectN; ++i)
        if (!strcmp(g_rects[i].id, id)) { *x = (int)((g_rects[i].x0 + g_rects[i].x1) * 0.5f); *y = (int)((g_rects[i].y0 + g_rects[i].y1) * 0.5f); return true; }
    return false;
}

void Shutdown() {
    if (!g_ready) return;
    ImGui_ImplDX11_Shutdown();
    ImGui::DestroyContext();
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    g_ready = false; g_visible = false;
}

}  // namespace OverlayUi
#endif
