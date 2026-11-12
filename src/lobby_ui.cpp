// Lobby UI (slices 6 + panel). See lobby_ui.h.
//   Panel: a small native window owned by the game window - "Host a game", your code + Copy, a box for a
//          friend's code + Join, and the link status. Opens by itself when the link comes up in a lobby
//          mode; F6 shows/hides it. It is a separate top-level window (not drawn inside the 3D frame), so it
//          needs no graphics hook; AC4's "fullscreen" is a WS_POPUP window, not an exclusive mode switch
//          (windowed.cpp), so an owned window stays above it.
//   Title: "<game title> | AC4Coop <status>", refreshed at most once a second.
//   Keys (game or panel focused only - GetAsyncKeyState is system-wide): F6 panel, F7 copy host code,
//          F8 join with the code on the clipboard.
// The game window is this process's largest visible top-level window (no engine handle is used).
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <process.h>
#include <cstdio>
#include <cstring>
#include "lobby_ui.h"
#include "net.h"

#pragma comment(lib, "gdi32.lib")   // panel fonts (CreateFontA)

namespace {

HWND  g_wnd = nullptr;
char  g_baseTitle[256] = {0};   // the game's own title, captured once
char  g_lastTitle[512] = {0};
char  g_copiedCode[16] = {0};   // the host code last put on the clipboard
DWORD g_lastTitleTick = 0;
bool  g_f6prev = false, g_f7prev = false, g_f8prev = false;

// ---- panel state (panel thread owns the window; other threads only post messages to it) ----
HWND  g_panel = nullptr;
volatile LONG g_panelStarted = 0;
HWND  hCodeLbl, hCode, hCopy, hHost, hJoinLbl, hEdit, hJoin, hStatus;
HFONT g_fontBig = nullptr, g_fontUi = nullptr;
const UINT WM_APP_TOGGLE = WM_APP + 1;
enum { ID_COPY = 101, ID_HOST, ID_JOIN, ID_EDIT };
char  g_panelErr[160] = {0};    // last Host/Join refusal, shown under the status until the next action

void Log(const char* s) { Net::Log(s); }   // net.cpp owns net_log.txt (opened deny-write)

struct FindCtx { DWORD pid; HWND best; LONG area; };
BOOL CALLBACK FindProc(HWND h, LPARAM lp) {
    FindCtx* c = (FindCtx*)lp;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER) || h == g_panel) return TRUE;
    RECT r; if (!GetWindowRect(h, &r)) return TRUE;
    const LONG a = (r.right - r.left) * (r.bottom - r.top);
    if (a > c->area) { c->area = a; c->best = h; }
    return TRUE;
}

HWND GameWindow() {
    if (g_wnd && IsWindow(g_wnd)) return g_wnd;
    FindCtx c{ GetCurrentProcessId(), nullptr, 0 };
    EnumWindows(FindProc, (LPARAM)&c);
    g_wnd = c.best;
    if (g_wnd && !g_baseTitle[0]) {
        DWORD_PTR r = 0;
        SendMessageTimeoutA(g_wnd, WM_GETTEXT, sizeof(g_baseTitle), (LPARAM)g_baseTitle, SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &r);
        g_baseTitle[sizeof(g_baseTitle) - 1] = 0;
    }
    return g_wnd;
}

bool OurFocus() {   // the game window or the panel is in front
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

bool CopyToClipboard(HWND owner, const char* text) {
    if (!OpenClipboard(owner)) return false;
    bool ok = false;
    EmptyClipboard();
    const size_t n = strlen(text) + 1;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    if (g) {
        void* p = GlobalLock(g);
        if (p) { memcpy(p, text, n); GlobalUnlock(g); ok = SetClipboardData(CF_TEXT, g) != nullptr; }
        if (!ok) GlobalFree(g);   // on success the clipboard owns it
    }
    CloseClipboard();
    return ok;
}

void Trim(char* out) {
    char* s = out; while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') ++s;
    if (s != out) memmove(out, s, strlen(s) + 1);
    size_t L = strlen(out); while (L && (out[L - 1] == ' ' || out[L - 1] == '\t' || out[L - 1] == '\r' || out[L - 1] == '\n')) out[--L] = 0;
}

bool ReadClipboard(HWND owner, char* out, size_t n) {
    out[0] = 0;
    if (!OpenClipboard(owner)) return false;
    bool ok = false;
    HANDLE h = GetClipboardData(CF_TEXT);
    if (h) {
        const char* p = (const char*)GlobalLock(h);
        if (p) { lstrcpynA(out, p, (int)n); ok = true; GlobalUnlock(h); }
    }
    CloseClipboard();
    Trim(out);
    return ok && out[0];
}

// ---- actions shared by the panel buttons and the keys ----
void DoJoin(const char* text, const char* via) {
    char err[160] = {0}, m[256];
    if (Net::JoinByCode(text, err, sizeof(err))) {
        g_panelErr[0] = 0;
        _snprintf(m, sizeof(m), "[Lobby] %s: joining %.20s\n", via, text);
    } else {
        lstrcpynA(g_panelErr, err, sizeof(g_panelErr));
        _snprintf(m, sizeof(m), "[Lobby] %s: \"%.40s\" not joined - %s\n", via, text, err);
    }
    m[sizeof(m) - 1] = 0; Log(m);
}
void DoHost(const char* via) {
    char err[160] = {0}, m[256];
    if (Net::StartHosting(err, sizeof(err))) { g_panelErr[0] = 0; _snprintf(m, sizeof(m), "[Lobby] %s: host\n", via); }
    else { lstrcpynA(g_panelErr, err, sizeof(g_panelErr)); _snprintf(m, sizeof(m), "[Lobby] %s: not hosting - %s\n", via, err); }
    m[sizeof(m) - 1] = 0; Log(m);
}
void DoCopy(HWND owner, const char* via) {
    char code[16], m[160];
    if (Net::HostCode(code)) {
        const bool ok = CopyToClipboard(owner, code);
        _snprintf(m, sizeof(m), "[Lobby] %s: host code %s %s\n", via, code, ok ? "copied" : "NOT copied (clipboard busy)");
    } else _snprintf(m, sizeof(m), "[Lobby] %s: no host code yet\n", via);
    m[sizeof(m) - 1] = 0; Log(m);
}

// ---- panel ----
void PlacePanel() {
    RECT pr; GetWindowRect(g_panel, &pr);
    const int w = pr.right - pr.left, h = pr.bottom - pr.top;
    RECT gr{};
    HWND gw = GameWindow();
    if (!gw || !GetWindowRect(gw, &gr)) { gr.left = 0; gr.top = 0; gr.right = GetSystemMetrics(SM_CXSCREEN); }
    SetWindowPos(g_panel, nullptr, gr.right - w - 24, gr.top + 48, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

void Refresh() {
    const int mode = Net::LobbyMode();   // 1 host, 2 join, 3 lobby
    char code[16] = {0};
    const bool haveCode = Net::HostCode(code);
    char cur[64] = {0}; GetWindowTextA(hCode, cur, sizeof(cur));
    const char* want = haveCode ? code : (mode == 1 ? "getting code..." : "-");
    if (strcmp(cur, want)) SetWindowTextA(hCode, want);
    EnableWindow(hCopy, haveCode);
    EnableWindow(hHost, mode == 3);
    SetWindowTextA(hHost, mode == 1 ? "Hosting - send your code to your friends" : "Host a game (get a code)");
    char st[200]; Net::LobbyStatus(st, sizeof(st));
    char full[400];
    _snprintf(full, sizeof(full), "%s%s%s", st, g_panelErr[0] ? "\r\n" : "", g_panelErr);
    full[sizeof(full) - 1] = 0;
    char now[400] = {0}; GetWindowTextA(hStatus, now, sizeof(now));
    if (strcmp(now, full)) SetWindowTextA(hStatus, full);
    const bool connected = strstr(st, "connected") != nullptr;
    EnableWindow(hEdit, (mode == 2 || mode == 3) && !connected);
    EnableWindow(hJoin, (mode == 2 || mode == 3) && !connected);
}

HWND Ctl(HWND parent, const char* cls, const char* text, DWORD style, int x, int y, int w, int h, int id, HFONT f) {
    HWND c = CreateWindowExA(cls[0] == 'E' ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
                             parent, (HMENU)(INT_PTR)id, GetModuleHandleA(nullptr), nullptr);
    SendMessageA(c, WM_SETFONT, (WPARAM)f, TRUE);
    return c;
}

LRESULT CALLBACK PanelProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        hCodeLbl = Ctl(h, "STATIC", "Your code", 0, 12, 10, 200, 18, 0, g_fontUi);
        hCode    = Ctl(h, "STATIC", "-", SS_LEFT, 12, 28, 236, 34, 0, g_fontBig);
        hCopy    = Ctl(h, "BUTTON", "Copy", BS_PUSHBUTTON, 256, 30, 84, 30, ID_COPY, g_fontUi);
        hHost    = Ctl(h, "BUTTON", "Host a game (get a code)", BS_PUSHBUTTON, 12, 70, 328, 30, ID_HOST, g_fontUi);
        hJoinLbl = Ctl(h, "STATIC", "Friend's code", 0, 12, 112, 200, 18, 0, g_fontUi);
        hEdit    = Ctl(h, "EDIT", "", ES_AUTOHSCROLL | ES_UPPERCASE | WS_TABSTOP, 12, 132, 236, 28, ID_EDIT, g_fontBig);
        hJoin    = Ctl(h, "BUTTON", "Join", BS_DEFPUSHBUTTON | WS_TABSTOP, 256, 131, 84, 30, ID_JOIN, g_fontUi);
        hStatus  = Ctl(h, "STATIC", "", SS_LEFT, 12, 170, 328, 44, 0, g_fontUi);
        SendMessageA(hEdit, EM_LIMITTEXT, 24, 0);
        SetTimer(h, 1, 300, nullptr);
        return 0;
    case WM_TIMER:
        Refresh();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_COPY) DoCopy(h, "panel Copy");
        else if (LOWORD(wp) == ID_HOST) DoHost("panel Host");
        else if (LOWORD(wp) == ID_JOIN || (LOWORD(wp) == IDOK)) {
            char t[64] = {0}; GetWindowTextA(hEdit, t, sizeof(t)); Trim(t);
            DoJoin(t, "panel Join");
        }
        Refresh();
        return 0;
    case WM_APP_TOGGLE:
        if (IsWindowVisible(h) && wp == 0) ShowWindow(h, SW_HIDE);
        else { PlacePanel(); ShowWindow(h, SW_SHOW); SetForegroundWindow(h); if (IsWindowEnabled(hEdit)) SetFocus(hEdit); }
        return 0;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);   // closing only hides it; F6 brings it back
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

unsigned __stdcall PanelThread(void*) {
    g_fontBig = CreateFontA(-24, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, "Consolas");
    g_fontUi  = CreateFontA(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    WNDCLASSA wc{};
    wc.lpfnWndProc = PanelProc; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "AC4CoopLobbyPanel";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    RECT r{ 0, 0, 352, 222 };
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectEx(&r, style, FALSE, WS_EX_TOOLWINDOW);
    // Owned by the game window: always above it, minimised with it; not topmost over other programs.
    g_panel = CreateWindowExA(WS_EX_TOOLWINDOW, wc.lpszClassName, "AC4Coop lobby  (F6)", style, 0, 0,
                              r.right - r.left, r.bottom - r.top, GameWindow(), nullptr, wc.hInstance, nullptr);
    if (!g_panel) { Log("[Lobby] panel: window creation failed\n"); return 0; }
    Log("[Lobby] panel: open (F6 shows/hides it)\n");
    PostMessageA(g_panel, WM_APP_TOGGLE, 1, 0);   // show once at start
    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageA(g_panel, &msg)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    }
    return 0;
}

}  // namespace

namespace LobbyUi {

void Tick() {
    if (!Net::Enabled()) return;
    const int mode = Net::LobbyMode();
    if (mode == 0) return;   // manual mode: no UI at all
    HWND w = GameWindow();
    char m[256];

    // The panel: one UI thread, started once the game window exists.
    if (w && !InterlockedExchange(&g_panelStarted, 1)) _beginthreadex(nullptr, 0, PanelThread, nullptr, 0, nullptr);

    // Host: copy the code once whenever it changes (LAN code first, then the internet / room code).
    if (mode == 1) {
        char code[16];
        if (Net::HostCode(code) && strcmp(code, g_copiedCode) != 0) {
            const bool ok = CopyToClipboard(w, code);
            _snprintf(m, sizeof(m), "[Lobby] host code %s %s\n", code, ok ? "copied to the clipboard" : "NOT copied (clipboard busy)");
            m[sizeof(m) - 1] = 0; Log(m);
            if (ok) lstrcpynA(g_copiedCode, code, sizeof(g_copiedCode));
        }
    }

    // Keys (edge-triggered, our windows focused).
    const bool focus = OurFocus();
    const bool f6 = focus && (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    const bool f7 = focus && (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    const bool f8 = focus && (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f6 && !g_f6prev && g_panel) PostMessageA(g_panel, WM_APP_TOGGLE, 0, 0);
    if (f7 && !g_f7prev && mode == 1) DoCopy(w, "F7");
    if (f8 && !g_f8prev) {
        char clip[128] = {0};
        if (mode == 1) Log("[Lobby] F8 ignored: this game is the host\n");
        else if (!ReadClipboard(w, clip, sizeof(clip))) Log("[Lobby] F8: the clipboard holds no text\n");
        else DoJoin(clip, "F8");
    }
    g_f6prev = f6; g_f7prev = f7; g_f8prev = f8;

    // Title: refresh at most once a second, and only when the text changes.
    const DWORD now = GetTickCount();
    if (w && now - g_lastTitleTick >= 1000) {
        g_lastTitleTick = now;
        char st[160]; Net::LobbyStatus(st, sizeof(st));
        char t[512];
        _snprintf(t, sizeof(t), "%s | AC4Coop %s", g_baseTitle[0] ? g_baseTitle : "AC4", st);
        t[sizeof(t) - 1] = 0;
        if (strcmp(t, g_lastTitle) != 0) {
            DWORD_PTR r = 0;
            // Timeout: the game's message pump may be busy loading; the poll thread must never stall.
            if (SendMessageTimeoutA(w, WM_SETTEXT, 0, (LPARAM)t, SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &r))
                lstrcpynA(g_lastTitle, t, sizeof(g_lastTitle));
        }
    }
}

}  // namespace LobbyUi
