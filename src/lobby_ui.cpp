// Lobby UI (slice 6). See lobby_ui.h.
//   host: the code goes into the window title and is copied to the clipboard once (again with F7).
//   join: F8 reads a code from the clipboard and joins that host (Net::JoinByCode).
// Keys act only while the game window has focus: GetAsyncKeyState is system-wide, so F8 pressed in a
// chat window would otherwise start a join. The game window is this process's largest visible
// top-level window (no engine handle is used; the plugin never captured the HWND).
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "lobby_ui.h"
#include "net.h"

namespace {

HWND  g_wnd = nullptr;
char  g_baseTitle[256] = {0};   // the game's own title, captured once
char  g_lastTitle[512] = {0};
char  g_copiedCode[16] = {0};   // the host code last put on the clipboard
DWORD g_lastTitleTick = 0;
bool  g_f7prev = false, g_f8prev = false;

void Log(const char* s) { Net::Log(s); }   // net.cpp owns net_log.txt (opened deny-write)

struct FindCtx { DWORD pid; HWND best; LONG area; };
BOOL CALLBACK FindProc(HWND h, LPARAM lp) {
    FindCtx* c = (FindCtx*)lp;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
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

bool GameHasFocus() {
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
    // Trim surrounding whitespace / newlines (chat apps add them).
    char* s = out; while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') ++s;
    if (s != out) memmove(out, s, strlen(s) + 1);
    size_t L = strlen(out); while (L && (out[L - 1] == ' ' || out[L - 1] == '\t' || out[L - 1] == '\r' || out[L - 1] == '\n')) out[--L] = 0;
    return ok && out[0];
}

}  // namespace

namespace LobbyUi {

void Tick() {
    if (!Net::Enabled()) return;
    const int mode = Net::LobbyMode();
    if (mode == 0) return;   // manual mode: no UI at all
    HWND w = GameWindow();
    char m[256];

    // Host: copy the code once whenever it changes (LAN code first, the internet code when it arrives).
    if (mode == 1) {
        char code[16];
        if (Net::HostCode(code) && strcmp(code, g_copiedCode) != 0) {
            const bool ok = CopyToClipboard(w, code);
            _snprintf(m, sizeof(m), "[Lobby] host code %s %s\n", code, ok ? "copied to the clipboard" : "NOT copied (clipboard busy)");
            m[sizeof(m) - 1] = 0; Log(m);
            if (ok) lstrcpynA(g_copiedCode, code, sizeof(g_copiedCode));
        }
    }

    // Keys (edge-triggered, focus required).
    const bool focus = GameHasFocus();
    const bool f7 = focus && (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    const bool f8 = focus && (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f7 && !g_f7prev && mode == 1) {
        char code[16];
        if (Net::HostCode(code)) {
            const bool ok = CopyToClipboard(w, code);
            _snprintf(m, sizeof(m), "[Lobby] F7: host code %s %s\n", code, ok ? "copied" : "NOT copied (clipboard busy)");
        } else _snprintf(m, sizeof(m), "[Lobby] F7: no host code yet\n");
        m[sizeof(m) - 1] = 0; Log(m);
    }
    if (f8 && !g_f8prev) {
        char clip[128] = {0}, err[128] = {0};
        if (mode != 2) _snprintf(m, sizeof(m), "[Lobby] F8 ignored: this game is the host ([Net] Mode=host)\n");
        else if (!ReadClipboard(w, clip, sizeof(clip))) _snprintf(m, sizeof(m), "[Lobby] F8: the clipboard holds no text\n");
        else if (!Net::JoinByCode(clip, err, sizeof(err))) _snprintf(m, sizeof(m), "[Lobby] F8: \"%.40s\" not joined - %s\n", clip, err);
        else _snprintf(m, sizeof(m), "[Lobby] F8: joining with the code from the clipboard\n");
        m[sizeof(m) - 1] = 0; Log(m);
    }
    g_f7prev = f7; g_f8prev = f8;

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
