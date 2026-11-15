#pragma once
// In-game co-op panel drawn with RmlUi (docs/design/eos-transport.md, "In-game overlay").
// This is the ONLY file pair that knows RmlUi. The hook layer (overlay.cpp) gives it a D3D11 device, the game
// window, a render target and window messages; swapping the UI library replaces overlay_ui.* alone.
// Built only when the RmlUi x86 libraries exist (third_party\rmlui\build_rmlui.cmd -> local\rmlui-x86); without
// them AC4COOP_OVERLAY is 0 and every function below is a no-op that reports "not built".
#include <windows.h>

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
};

// Panel buttons -> the owner (overlay.cpp / net). action: "host" | "join" | "copy" | "stop"; arg: code for "join".
typedef void (*ActionFn)(const char* action, const char* arg);
typedef void (*LogFn)(const char* line);

bool Built();   // false when compiled without RmlUi
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
