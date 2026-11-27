#pragma once
// In-game co-op panel (docs/design/overlay.md). The hook layer (overlay.cpp) gives it a D3D11 device, the game
// window, a render target and window messages; the UI library lives only behind this interface.
// Implementations (exactly one is compiled in, chosen in AC4Coop.vcxproj):
//   overlay_ui_imgui.cpp  Dear ImGui (third_party\imgui, vendored) - the DEFAULT        AC4COOP_OVERLAY=1
//   overlay_ui.cpp        RmlUi, opt-in: /p:Ac4UiRmlUi=1 AND local\rmlui-x86 built    AC4COOP_OVERLAY=1 + AC4COOP_UI_RMLUI=1
//   overlay_ui.cpp stub   neither available: every function is a no-op that reports "not built"
#include <windows.h>
#include <stdint.h>

struct ID3D11Device;
struct ID3D11RenderTargetView;

namespace OverlayUi {

// What the panel shows. Strings are plain ASCII; empty means "-".
struct Status {
    char role[16];    // "host" | "join" (from the ini; the panel offers only that action)
    char state[32];   // bridge state: off | starting | hosting | joined | connected | stopped | failed-*
    char code[16];    // lobby code (host: generated, join: what was entered)
    char link[16];    // DIRECT | RELAYED | - (EOS established event)
    char peer[48];    // mod link state line, e.g. "peer up 48 ms"
    char note[96];    // last action result / hint ("a lobby code is 6 letters/digits"); empty = none
    // Read-only rows (filled on the overlay thread from existing mod globals; never from the render thread).
    uint32_t proto;   // NET_PROTO_VER of this build
    char commit[16];  // [Build] commit=<short>
    char asi[16];     // [Build] asi=<first 12 hex of the sha256>
    int  puppet;      // 1 = a puppet entity exists (Phase1_PuppetEntity != nullptr)
    int  shipsBound;  // ship class-bind table entries (Phase1_ShipBindCount)
    int  guardsBound; // character (kind 2) records bound to a local body in the current 5 s window (Phase1_GuardBindCount)
};

// Panel buttons -> the owner (overlay.cpp / net). action: "host" | "join" | "copy" | "stop"; arg: code for "join".
typedef void (*ActionFn)(const char* action, const char* arg);
typedef void (*LogFn)(const char* line);

bool Built();          // false for the stub
const char* Name();    // "imgui" | "rmlui" | "none"
// uiDir: folder with overlay.rml, overlay.rcss and the fonts (absolute path). Returns false (and logs why) on failure.
bool Init(ID3D11Device* dev, HWND hwnd, int width, int height, const char* uiDir, ActionFn onAction, LogFn log);
void Resize(int width, int height);
// Updates and draws into rtv (the back buffer) when visible. The caller owns D3D state backup/restore.
void Render(ID3D11RenderTargetView* rtv);
// Feed a window message while visible. Returns true when the panel consumed it.
bool OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
void SetVisible(bool v);
bool Visible();
void SetStatus(const Status& s);
void Shutdown();
// Test support (tools/overlay_test): client-space centre of the element with this id; false if absent or hidden.
bool ElementCenter(const char* id, int* x, int* y);

}  // namespace OverlayUi
