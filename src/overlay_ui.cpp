// In-game co-op panel with RmlUi (see overlay_ui.h). The only RmlUi-aware translation unit in the ASI.
#define _CRT_SECURE_NO_WARNINGS
#include "overlay_ui.h"

#ifndef AC4COOP_OVERLAY
#define AC4COOP_OVERLAY 0
#endif

#if !AC4COOP_OVERLAY
namespace OverlayUi {
bool Built() { return false; }
bool Init(ID3D11Device*, HWND, int, int, const char*, ActionFn, LogFn log) { if (log) log("[Overlay] built without RmlUi (run third_party\\rmlui\\build_rmlui.cmd)\n"); return false; }
void Resize(int, int) {}
void Render(ID3D11RenderTargetView*) {}
bool OnMessage(HWND, UINT, WPARAM, LPARAM) { return false; }
void SetVisible(bool) {}
bool Visible() { return false; }
void SetStatus(const Status&) {}
void Shutdown() {}
bool ElementCenter(const char*, int*, int*) { return false; }
}
#else

#include <d3d11.h>
#include <d3d11_1.h>
#include <cstdio>
#include <cstring>
#include <RmlUi/Core.h>
#include "RmlUi_Platform_Win32.h"
#include "RmlUi_Renderer_DX11.h"

namespace {

OverlayUi::ActionFn g_onAction = nullptr;
OverlayUi::LogFn g_log = nullptr;
void Logf(const char* fmt, ...) {
    if (!g_log) return;
    char b[512]; va_list ap; va_start(ap, fmt); _vsnprintf(b, sizeof(b) - 1, fmt, ap); va_end(ap); b[sizeof(b) - 1] = 0;
    g_log(b);
}

// RmlUi's own log goes to ours (warnings and errors only: missing files, RML syntax).
struct SystemIf : SystemInterface_Win32 {
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        if (type <= Rml::Log::LT_WARNING || message.find("font face") != Rml::String::npos) Logf("[Overlay] rmlui: %s\n", message.c_str());
        return true;
    }
    void SetMouseCursor(const Rml::String&) override {}   // the game owns the OS cursor; we draw our own element
};

SystemIf* g_sys = nullptr;
RenderInterface_DX11* g_render = nullptr;
TextInputMethodEditor_Win32* g_ime = nullptr;
Rml::Context* g_ctx = nullptr;
Rml::ElementDocument* g_doc = nullptr;
Rml::Element* g_cursor = nullptr;
Rml::DataModelHandle g_model;
bool g_visible = false;
int g_w = 0, g_h = 0;

// Derived flags are computed here, not in RML expressions, so the markup stays plain comparisons.
struct Model {
    Rml::String role = "-", state = "off", code = "-", link = "-", peer = "-", note, input;
    bool can_start = true, has_code = false, active = false, failed = false, good = false, no_role = true, show_host = false, show_join = false;
} g_m;

void Derive() {
    const bool off = g_m.state == "off" || g_m.state == "stopped";
    const bool failed = g_m.state.compare(0, 6, "failed") == 0;
    // role "any" = [Overlay] RoleFromPanel and nothing picked yet: both Host and Join are offered.
    const bool any = g_m.role == "any";
    const bool v[] = { off || failed, g_m.code != "-", !off && !failed, failed, g_m.state == "connected",
                       g_m.role != "host" && g_m.role != "join" && !any, g_m.role == "host" || any, g_m.role == "join" || any };
    bool* dst[] = { &g_m.can_start, &g_m.has_code, &g_m.active, &g_m.failed, &g_m.good, &g_m.no_role, &g_m.show_host, &g_m.show_join };
    const char* names[] = { "can_start", "has_code", "active", "failed", "good", "no_role", "show_host", "show_join" };
    for (int i = 0; i < 8; ++i)
        if (*dst[i] != v[i]) { *dst[i] = v[i]; g_model.DirtyVariable(names[i]); }
}

void OnAct(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
    if (args.empty() || !g_onAction) return;
    const Rml::String a = args[0].Get<Rml::String>();
    Rml::String code = g_m.input;
    for (auto& c : code) c = (char)toupper((unsigned char)c);
    g_onAction(a.c_str(), code.c_str());
}

}  // namespace

namespace OverlayUi {

bool Built() { return true; }

bool Init(ID3D11Device* dev, HWND hwnd, int w, int h, const char* uiDir, ActionFn onAction, LogFn log) {
    g_onAction = onAction; g_log = log;
    if (g_ctx) return true;
    // RenderInterface_DX11 uses ID3D11Device1 without checking the QueryInterface result
    // (Backends/RmlUi_Renderer_DX11.cpp, constructor), so refuse here instead of crashing there.
    ID3D11Device1* d1 = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&d1)) || !d1) { Logf("[Overlay] no ID3D11Device1 - overlay off\n"); return false; }
    d1->Release();

    g_sys = new SystemIf(); g_sys->SetWindow(hwnd);
    g_render = new RenderInterface_DX11(dev);
    g_render->SetViewport(w, h);
    g_ime = new TextInputMethodEditor_Win32();
    Rml::SetSystemInterface(g_sys);
    Rml::SetRenderInterface(g_render);
    Rml::SetTextInputHandler(g_ime);
    if (!Rml::Initialise()) { Logf("[Overlay] Rml::Initialise failed\n"); return false; }

    char p[MAX_PATH];
    const char* fonts[] = { "LatoLatin-Regular.ttf", "LatoLatin-Bold.ttf" };
    for (const char* f : fonts) {
        _snprintf(p, sizeof(p), "%s\\%s", uiDir, f); p[sizeof(p) - 1] = 0;
        if (!Rml::LoadFontFace(p)) Logf("[Overlay] font not loaded: %s\n", p);
    }
    g_ctx = Rml::CreateContext("coop", Rml::Vector2i(w, h));
    if (!g_ctx) { Logf("[Overlay] CreateContext failed\n"); return false; }
    g_w = w; g_h = h;

    if (Rml::DataModelConstructor c = g_ctx->CreateDataModel("coop")) {
        c.Bind("role", &g_m.role); c.Bind("state", &g_m.state); c.Bind("code", &g_m.code);
        c.Bind("link", &g_m.link); c.Bind("peer", &g_m.peer); c.Bind("note", &g_m.note); c.Bind("input", &g_m.input);
        c.Bind("can_start", &g_m.can_start); c.Bind("has_code", &g_m.has_code); c.Bind("active", &g_m.active);
        c.Bind("failed", &g_m.failed); c.Bind("good", &g_m.good); c.Bind("no_role", &g_m.no_role);
        c.Bind("show_host", &g_m.show_host); c.Bind("show_join", &g_m.show_join);
        c.BindEventCallback("act", &OnAct);
        g_model = c.GetModelHandle();
    }
    _snprintf(p, sizeof(p), "%s\\overlay.rml", uiDir); p[sizeof(p) - 1] = 0;
    g_doc = g_ctx->LoadDocument(p);
    if (!g_doc) { Logf("[Overlay] document not loaded: %s\n", p); return false; }
    g_cursor = g_doc->GetElementById("cursor");
    g_doc->Hide();
    Logf("[Overlay] RmlUi %s ready %dx%d ui=%s\n", Rml::GetVersion().c_str(), w, h, uiDir);
    return true;
}

void Resize(int w, int h) {
    if (!g_ctx || (w == g_w && h == g_h) || w < 1 || h < 1) return;
    g_w = w; g_h = h;
    g_render->SetViewport(w, h);
    g_ctx->SetDimensions(Rml::Vector2i(w, h));
}

void Render(ID3D11RenderTargetView* rtv) {
    if (!g_ctx || !g_visible || !rtv) return;
    g_ctx->Update();   // the software cursor element is moved in OnMessage (WM_MOUSEMOVE)
    g_render->BeginFrame();
    g_ctx->Render();
    g_render->EndFrame(rtv);
}

bool OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!g_ctx || !g_visible) return false;
    if (msg == WM_MOUSEMOVE && g_cursor) {
        char v[32];
        _snprintf(v, sizeof(v), "%dpx", (int)(short)LOWORD(lp)); g_cursor->SetProperty("left", v);
        _snprintf(v, sizeof(v), "%dpx", (int)(short)HIWORD(lp)); g_cursor->SetProperty("top", v);
    }
    RmlWin32::WindowProcedure(g_ctx, *g_ime, hwnd, msg, wp, lp);
    switch (msg) {   // everything the game would read as gameplay input while the panel is open
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL: case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
        case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_INPUT: case WM_LBUTTONDBLCLK: case WM_IME_CHAR:
        case WM_IME_COMPOSITION: case WM_IME_STARTCOMPOSITION: case WM_IME_ENDCOMPOSITION:
            return true;
    }
    return false;
}

void SetVisible(bool v) {
    if (!g_doc || v == g_visible) return;
    g_visible = v;
    if (v) g_doc->Show(); else g_doc->Hide();
}
bool Visible() { return g_visible; }

void SetStatus(const Status& s) {
    if (!g_model) return;
    auto set = [](Rml::String& dst, const char* src, const char* name) {
        const Rml::String v = (src && *src) ? src : "-";
        if (dst != v) { dst = v; g_model.DirtyVariable(name); }
    };
    set(g_m.role, s.role, "role"); set(g_m.state, s.state, "state"); set(g_m.code, s.code, "code");
    set(g_m.link, s.link, "link"); set(g_m.peer, s.peer, "peer");
    if (g_m.note != s.note) { g_m.note = s.note; g_model.DirtyVariable("note"); }
    if (g_m.state == "-") g_m.state = "off";
    Derive();
}

bool ElementCenter(const char* id, int* x, int* y) {
    if (!g_doc) return false;
    g_ctx->Update();
    Rml::Element* e = g_doc->GetElementById(id);
    if (!e || !e->IsVisible()) return false;
    const Rml::Vector2f o = e->GetAbsoluteOffset(Rml::BoxArea::Border), s = e->GetBox().GetSize(Rml::BoxArea::Border);
    *x = (int)(o.x + s.x * 0.5f); *y = (int)(o.y + s.y * 0.5f);
    return true;
}

void Shutdown() {
    if (g_ctx) { Rml::Shutdown(); g_ctx = nullptr; g_doc = nullptr; g_cursor = nullptr; }
    delete g_render; g_render = nullptr;
    delete g_ime; g_ime = nullptr;
    delete g_sys; g_sys = nullptr;
}

}  // namespace OverlayUi
#endif
