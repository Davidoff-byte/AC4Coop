// AC4Coop Launcher: the ONLY way the co-op mod runs. It finds the game, checks its build, installs/updates the mod
// files, arms this one start and launches the game. Host / Join happen in the game, in the co-op panel (F10).
// docs/design/launcher.md. Dear ImGui (third_party/imgui) on Win32 + D3D11 in its own window - no game hooking.
//
// Separation from the plain game: on install the launcher writes [Launcher] RequireArm=1, and before each start it
// writes <game>\AC4Coop.armed. The ASI installs nothing on a start without a fresh marker, so Play in Steam / Ubisoft
// Connect is the unmodded game. The marker must survive the store hand-off (measured 2026-11-15: a direct
// AC4BFSP.exe start exits within ~1 s and UbisoftGameLauncher.exe starts the real game ~3 s later); the live game
// removes it after 60 s (src/dllmain.cpp, LaunchArmed / DisarmLaunch).
//
// Loader: Ultimate ASI Loader as dinput8.dll. If another mod already owns a dinput8.dll that is not Ultimate ASI
// Loader, ours goes in as version.dll instead (both names are on the loader's Win32 list and AC4BFSP.exe imports both).
//
// Test flags (unattended checks; normal users start it with no arguments):
//   --game-dir DIR  --payload DIR  --no-launch  --auto-play (install + arm + launch once)  --exit-after S
//   --report FILE (state as key=value)  --shot FILE.bmp (window image after 3 s)
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include "netstate.h"   // NET_PROTO_VER only (header-only constants)

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "version.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

// ---------------------------------------------------------------- settings + args
struct Args { std::string gameDir, report, shot, payload; int exitAfter = 0; bool noLaunch = false, autoPlay = false; } A;
char g_exeDir[MAX_PATH], g_settings[MAX_PATH], g_gameDir[MAX_PATH] = {0};

void LoadSettings() {
    GetModuleFileNameA(nullptr, g_exeDir, MAX_PATH); *strrchr(g_exeDir, '\\') = 0;
    _snprintf(g_settings, MAX_PATH, "%s\\launcher.ini", g_exeDir);
    GetPrivateProfileStringA("Launcher", "GameDir", "", g_gameDir, MAX_PATH, g_settings);
}
void LogLine(const char* fmt, ...) {   // launcher_log.txt next to the exe
    char path[MAX_PATH]; _snprintf(path, MAX_PATH, "%s\\launcher_log.txt", g_exeDir);
    FILE* f = fopen(path, "a"); if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fputc('\n', f); fclose(f);
}
void SaveSettings() { if (A.gameDir.empty()) WritePrivateProfileStringA("Launcher", "GameDir", g_gameDir, g_settings); }

// ---------------------------------------------------------------- files
bool FileExists(const std::string& p) { DWORD a = GetFileAttributesA(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
bool DirExists(const std::string& p) { DWORD a = GetFileAttributesA(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
std::string Join(const std::string& dir, const char* leaf) { return dir + "\\" + leaf; }
std::string PayloadDir() { return A.payload.empty() ? Join(g_exeDir, "files") : A.payload; }

bool Sha256File(const std::string& path, char hex[65]) {
    hex[0] = 0;
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr; UCHAR dig[32];
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 && BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0;
    std::vector<UCHAR> buf(1 << 20); DWORD got = 0;
    while (ok && ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got) ok = BCryptHashData(h, buf.data(), got, 0) == 0;
    ok = ok && BCryptFinishHash(h, dig, 32, 0) == 0;
    if (h) BCryptDestroyHash(h); if (alg) BCryptCloseAlgorithmProvider(alg, 0); CloseHandle(f);
    if (ok) for (int i = 0; i < 32; ++i) sprintf(hex + i * 2, "%02x", dig[i]);
    return ok;
}
bool SameFile(const std::string& a, const std::string& b) { char x[65], y[65]; return Sha256File(a, x) && Sha256File(b, y) && !strcmp(x, y); }

// known_builds.txt: "<sha256> <label>" lines; '#' comments.
bool KnownBuild(const char* sha, std::string* label) {
    FILE* f = fopen(Join(PayloadDir(), "known_builds.txt").c_str(), "r");
    if (!f) return false;
    char line[512]; bool ok = false;
    while (!ok && fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || strlen(line) < 64) continue;
        if (_strnicmp(line, sha, 64) == 0) { ok = true; if (label) { *label = line + 65; while (!label->empty() && (label->back() == '\n' || label->back() == '\r')) label->pop_back(); } }
    }
    fclose(f);
    return ok;
}

// Is this file Ultimate ASI Loader? (version resource ProductName / FileDescription)
bool IsAsiLoader(const std::string& path) {
    DWORD h = 0, n = GetFileVersionInfoSizeA(path.c_str(), &h);
    if (!n) return false;
    std::vector<char> buf(n);
    if (!GetFileVersionInfoA(path.c_str(), 0, n, buf.data())) return false;
    struct { WORD lang, cp; }* tr = nullptr; UINT tn = 0;
    if (!VerQueryValueA(buf.data(), "\\VarFileInfo\\Translation", (void**)&tr, &tn) || tn < 4) return false;
    for (const char* key : { "ProductName", "FileDescription" }) {
        char q[96]; _snprintf(q, sizeof(q), "\\StringFileInfo\\%04x%04x\\%s", tr->lang, tr->cp, key);
        char* v = nullptr; UINT vn = 0;
        if (VerQueryValueA(buf.data(), q, (void**)&v, &vn) && v && (strstr(v, "ASI Loader") || strstr(v, "ASI-Loader"))) return true;
    }
    return false;
}

// Steam libraries (libraryfolders.vdf "path" entries) and Ubisoft Connect installs (registry InstallDir).
std::vector<std::string> Candidates() {
    std::vector<std::string> out;
    char steam[MAX_PATH] = {0}; DWORD n = sizeof(steam);
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", RRF_RT_REG_SZ, nullptr, steam, &n) == ERROR_SUCCESS) {
        std::vector<std::string> libs{ steam };
        FILE* f = fopen((std::string(steam) + "\\steamapps\\libraryfolders.vdf").c_str(), "r");
        if (f) {
            char line[1024];
            while (fgets(line, sizeof(line), f)) {
                const char* k = strstr(line, "\"path\""); if (!k) continue;
                const char* a = strchr(k + 6, '"'); if (!a) continue; const char* b = strchr(a + 1, '"'); if (!b) continue;
                std::string p(a + 1, b); std::string q; for (size_t i = 0; i < p.size(); ++i) { if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') ++i; q += p[i]; }
                libs.push_back(q);
            }
            fclose(f);
        }
        for (auto& l : libs) out.push_back(l + "\\steamapps\\common\\Assassin's Creed IV Black Flag");
    }
    HKEY k;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Ubisoft\\Launcher\\Installs", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        char sub[64]; for (DWORD i = 0; RegEnumKeyA(k, i, sub, sizeof(sub)) == ERROR_SUCCESS; ++i) {
            char dir[MAX_PATH] = {0}; DWORD dn = sizeof(dir);
            if (RegGetValueA(k, sub, "InstallDir", RRF_RT_REG_SZ, nullptr, dir, &dn) == ERROR_SUCCESS) {
                std::string d(dir); for (auto& c : d) if (c == '/') c = '\\';
                while (!d.empty() && d.back() == '\\') d.pop_back();
                out.push_back(d);
            }
        }
        RegCloseKey(k);
    }
    return out;
}
bool FindGame() {
    for (auto& c : Candidates()) if (FileExists(c + "\\AC4BFSP.exe")) { lstrcpynA(g_gameDir, c.c_str(), MAX_PATH); return true; }
    return false;
}
bool GameRunning() {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32 e{ sizeof(e) }; bool found = false;
    for (BOOL ok = Process32First(s, &e); ok && !found; ok = Process32Next(s, &e)) found = _stricmp(e.szExeFile, "AC4BFSP.exe") == 0;
    CloseHandle(s);
    return found;
}

// ---------------------------------------------------------------- state
struct GameInfo {
    bool found = false, known = false, payloadOk = false, upToDate = false;
    std::string label, exeSha, loaderName, loaderNote, missing;
} G;

// Which file name our loader uses in this game folder: dinput8.dll unless another (non-ASI-Loader) dinput8.dll is there.
std::string PickLoaderName(std::string* note) {
    const std::string d8 = Join(g_gameDir, "dinput8.dll"), ver = Join(g_gameDir, "version.dll");
    if (!FileExists(d8) || IsAsiLoader(d8)) return "dinput8.dll";
    if (!FileExists(ver) || IsAsiLoader(ver)) { *note = "Another mod owns dinput8.dll - the co-op loader goes in as version.dll"; return "version.dll"; }
    *note = "Both dinput8.dll and version.dll belong to other mods - remove one of them to install co-op";
    return "";
}

// Payload: files the launcher ships (src relative to files\) and where they go (dst relative to the game folder).
struct Item { std::string src, dst; };
std::vector<Item> PayloadItems(const std::string& loader) {
    std::vector<Item> v = { { "AC4Coop.asi", "AC4Coop.asi" }, { "known_builds.txt", "known_builds.txt" } };
    if (!loader.empty()) v.push_back({ "dinput8.dll", loader });
    for (const char* dir : { "eos", "coop_ui" }) {
        WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((Join(PayloadDir(), dir) + "\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) v.push_back({ std::string(dir) + "\\" + fd.cFileName, std::string(dir) + "\\" + fd.cFileName }); }
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return v;
}

void CheckGame() {
    G = GameInfo{};
    const std::string exe = Join(g_gameDir, "AC4BFSP.exe");
    G.found = g_gameDir[0] && FileExists(exe);
    char hex[65];
    if (G.found && Sha256File(exe, hex)) { G.exeSha = hex; G.known = KnownBuild(hex, &G.label); }
    G.payloadOk = FileExists(Join(PayloadDir(), "AC4Coop.asi")) && FileExists(Join(PayloadDir(), "dinput8.dll")) &&
                  FileExists(Join(PayloadDir(), "eos\\eos_bridge.exe")) && FileExists(Join(PayloadDir(), "eos\\eos.ini")) &&
                  FileExists(Join(PayloadDir(), "coop_ui\\overlay.rml"));
    if (!G.found) return;
    G.loaderName = PickLoaderName(&G.loaderNote);
    G.upToDate = !G.loaderName.empty();
    for (auto& it : PayloadItems(G.loaderName))
        if (!SameFile(Join(PayloadDir(), it.src.c_str()), Join(g_gameDir, it.dst.c_str()))) { G.upToDate = false; G.missing = it.dst; break; }
}

// ---------------------------------------------------------------- install + play
std::string g_msg; bool g_msgOk = false;
void Msg(bool ok, const std::string& m) { g_msg = m; g_msgOk = ok; LogLine("%s", m.c_str()); }

bool Install() {
    if (!G.payloadOk) { Msg(false, "This launcher's files\\ folder is incomplete - unzip the whole package again"); return false; }
    if (GameRunning()) { Msg(false, "Close Assassin's Creed IV first"); return false; }
    if (G.loaderName.empty()) { Msg(false, G.loaderNote); return false; }
    CreateDirectoryA(Join(g_gameDir, "eos").c_str(), nullptr);
    CreateDirectoryA(Join(g_gameDir, "coop_ui").c_str(), nullptr);
    for (auto& it : PayloadItems(G.loaderName)) {
        const std::string src = Join(PayloadDir(), it.src.c_str()), dst = Join(g_gameDir, it.dst.c_str());
        if (SameFile(src, dst)) continue;
        if (FileExists(dst)) CopyFileA(dst.c_str(), (dst + ".launcher-bak").c_str(), FALSE);
        if (!CopyFileA(src.c_str(), dst.c_str(), FALSE)) { Msg(false, "Could not write " + dst); return false; }
    }
    const std::string ini = Join(g_gameDir, "AC4Coop.ini"), tpl = Join(PayloadDir(), "AC4Coop.ini");
    if (!FileExists(ini) && FileExists(tpl)) CopyFileA(tpl.c_str(), ini.c_str(), TRUE);   // never overwrite the player's ini
    if (!FileExists(ini)) { Msg(false, "AC4Coop.ini could not be created in the game folder"); return false; }
    CopyFileA(ini.c_str(), (ini + ".launcher.bak").c_str(), FALSE);
    // The keys the co-op start needs; everything else in the player's ini is left alone.
    const char* kv[][3] = {
        { "Launcher", "RequireArm", "1" },        // plain Steam / Ubisoft starts stay unmodded
        { "Net", "Enabled", "1" }, { "Net", "Mode", "manual" }, { "Net", "Transport", "eos" }, { "Net", "EosAutoStart", "0" },
        { "Overlay", "Enable", "1" }, { "Overlay", "RoleFromPanel", "1" },
        // Co-op sync profile (role-independent; the role comes from the panel). Same set the party launcher wrote.
        { "Coop", "EntitySync", "1" }, { "Coop", "EntityFrameCadence", "1" }, { "Coop", "OwnUpdateGuards", "1" },
        { "Coop", "OwnUpdatePuppet", "1" }, { "Coop", "OwnPhysicsMs", "1" }, { "Coop", "MirrorActions", "1" },
        { "Coop", "MirrorActionsDeferred", "1" }, { "Coop", "GuardStaleReturn", "1" }, { "Coop", "GuardAnimFull", "1" },
        { "Coop", "GuardBlendFix", "1" }, { "Coop", "BlendTypeSafe", "1" }, { "Coop", "GuardPhaseLock", "1" },
        { "Coop", "GuardInterpBuffer", "1" }, { "Coop", "GuardInterpDelayMs", "120" }, { "Coop", "PuppetHostTime", "1" },
        { "Coop", "PuppetActionReassert", "1" }, { "Coop", "BindRadiusM", "60" }, { "Coop", "CrashContext", "1" },
        { "Coop", "GuardActionGate", "1" }, { "Coop", "FootOffsetFix", "1" },
    };
    for (auto& p : kv) WritePrivateProfileStringA(p[0], p[1], p[2], ini.c_str());
    CheckGame();
    Msg(true, "Co-op mod installed (" + G.loaderName + " loader)");
    return true;
}

bool g_launched = false;
bool Play() {
    if (!G.found) { Msg(false, "Find the game folder first"); return false; }
    if (GameRunning()) { Msg(false, "Assassin's Creed IV is already running - close it, then press Play co-op"); return false; }
    if (!G.upToDate && !Install()) return false;
    // Arm this start: the ASI checks the marker's age ([Launcher] ArmWindowSec, default 300 s).
    const std::string marker = Join(g_gameDir, "AC4Coop.armed");
    FILE* f = fopen(marker.c_str(), "w");
    if (!f) { Msg(false, "Could not write " + marker); return false; }
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "armed by AC4CoopLauncher %04u-%02u-%02u %02u:%02u:%02u\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    fclose(f);
    if (A.noLaunch) { Msg(true, "Test mode: installed and armed, game not started"); return true; }
    STARTUPINFOA si{ sizeof(si) }; PROCESS_INFORMATION pi{};
    const std::string exe = Join(g_gameDir, "AC4BFSP.exe");
    if (!CreateProcessA(exe.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, g_gameDir, &si, &pi)) {
        char b[128]; _snprintf(b, sizeof(b), "Could not start the game (error %lu)", GetLastError()); Msg(false, b); return false;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    g_launched = true;
    Msg(true, "Starting the game with co-op... press F10 in the game to host or join.");
    return true;
}

// ---------------------------------------------------------------- D3D11 + window
ID3D11Device* g_dev = nullptr; ID3D11DeviceContext* g_ctx = nullptr; IDXGISwapChain* g_sc = nullptr; ID3D11RenderTargetView* g_rtv = nullptr;
void CreateRTV() { ID3D11Texture2D* bb = nullptr; g_sc->GetBuffer(0, IID_PPV_ARGS(&bb)); g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv); bb->Release(); }
void CleanupRTV() { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } }
bool CreateDevice(HWND w) {
    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = w; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl; const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 2, D3D11_SDK_VERSION, &sd, &g_sc, &g_dev, &fl, &g_ctx);
    if (hr == DXGI_ERROR_UNSUPPORTED) hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, want, 2, D3D11_SDK_VERSION, &sd, &g_sc, &g_dev, &fl, &g_ctx);
    if (FAILED(hr)) return false;
    CreateRTV(); return true;
}
UINT g_resizeW = 0, g_resizeH = 0;
LRESULT WINAPI WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return true;
    if (m == WM_SIZE) { if (w != SIZE_MINIMIZED) { g_resizeW = LOWORD(l); g_resizeH = HIWORD(l); } return 0; }
    if (m == WM_SYSCOMMAND && (w & 0xfff0) == SC_KEYMENU) return 0;
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}
void SaveShot(const char* path) {   // backbuffer -> 32-bit BMP
    ID3D11Texture2D* bb = nullptr; g_sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d); d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr; g_dev->CreateTexture2D(&d, nullptr, &st); g_ctx->CopyResource(st, bb);
    D3D11_MAPPED_SUBRESOURCE map; g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &map);
    BITMAPINFOHEADER bi{ sizeof(bi), (LONG)d.Width, -(LONG)d.Height, 1, 32, BI_RGB };
    BITMAPFILEHEADER bf{ 0x4D42, (DWORD)(sizeof(bf) + sizeof(bi) + d.Width * d.Height * 4), 0, 0, (DWORD)(sizeof(bf) + sizeof(bi)) };
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(&bf, sizeof(bf), 1, f); fwrite(&bi, sizeof(bi), 1, f);
        std::vector<uint8_t> row(d.Width * 4);
        for (UINT y = 0; y < d.Height; ++y) { const uint8_t* s = (const uint8_t*)map.pData + y * map.RowPitch;
            for (UINT x = 0; x < d.Width; ++x) { row[x * 4] = s[x * 4 + 2]; row[x * 4 + 1] = s[x * 4 + 1]; row[x * 4 + 2] = s[x * 4]; row[x * 4 + 3] = 255; }
            fwrite(row.data(), row.size(), 1, f); }
        fclose(f); }
    g_ctx->Unmap(st, 0); st->Release(); bb->Release();
}

// ---------------------------------------------------------------- UI
void Style() {
    ImGuiStyle& s = ImGui::GetStyle(); ImGui::StyleColorsDark();
    s.WindowRounding = 0; s.FrameRounding = 4; s.GrabRounding = 4; s.FramePadding = ImVec2(12, 8); s.ItemSpacing = ImVec2(10, 10);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.07f, 0.05f, 1);
    c[ImGuiCol_Button] = ImVec4(0.79f, 0.64f, 0.36f, 1); c[ImGuiCol_ButtonHovered] = ImVec4(0.89f, 0.76f, 0.49f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.61f, 0.49f, 0.26f, 1); c[ImGuiCol_Text] = ImVec4(0.91f, 0.89f, 0.81f, 1);
    c[ImGuiCol_Separator] = ImVec4(0.29f, 0.25f, 0.17f, 1);
}
ImFont* g_big = nullptr; ImFont* g_mid = nullptr;
const ImVec4 kOk(0.56f, 0.82f, 0.54f, 1), kWarn(0.91f, 0.76f, 0.48f, 1), kBad(0.88f, 0.48f, 0.39f, 1), kDim(0.55f, 0.52f, 0.45f, 1), kGold(0.85f, 0.71f, 0.42f, 1);

void Ui() {
    ImGui::PushFont(g_big); ImGui::TextColored(kGold, "AC4 CO-OP"); ImGui::PopFont();
    ImGui::TextColored(kDim, "Starts Assassin's Creed IV with the co-op mod. Host or join in the game: press F10.");
    ImGui::Separator();

    ImGui::TextColored(kDim, "Game folder");
    ImGui::TextWrapped("%s", g_gameDir[0] ? g_gameDir : "(not found)");
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.23f, 0.20f, 0.15f, 1)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.31f, 0.27f, 0.20f, 1));
    if (ImGui::Button("Find")) { if (!FindGame()) Msg(false, "Not found in Steam or Ubisoft Connect libraries - use Browse"); SaveSettings(); CheckGame(); }
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        char file[MAX_PATH] = "AC4BFSP.exe"; OPENFILENAMEA o{ sizeof(o) }; o.lpstrFilter = "AC4BFSP.exe\0AC4BFSP.exe\0"; o.lpstrFile = file; o.nMaxFile = MAX_PATH; o.Flags = OFN_FILEMUSTEXIST;
        if (GetOpenFileNameA(&o)) { *strrchr(file, '\\') = 0; lstrcpynA(g_gameDir, file, MAX_PATH); SaveSettings(); CheckGame(); }
    }
    ImGui::PopStyleColor(2);

    bool canPlay = G.found && G.known && G.payloadOk && (G.upToDate || !G.loaderName.empty());
    if (!G.found) ImGui::TextColored(kBad, "AC4BFSP.exe not found");
    else {
        if (G.known) ImGui::TextColored(kOk, "Supported game build (%s)", G.label.c_str());
        else ImGui::TextColored(kBad, "Unsupported game build (sha256 %.12s...) - co-op cannot run on it", G.exeSha.c_str());
        if (!G.payloadOk) ImGui::TextColored(kBad, "This launcher's files\\ folder is incomplete - unzip the whole package again");
        else if (G.upToDate) ImGui::TextColored(kOk, "Co-op mod installed and up to date (%s loader)", G.loaderName.c_str());
        else if (!G.loaderName.empty()) ImGui::TextColored(kWarn, "Co-op mod will be installed or updated when you press Play (%s)", G.missing.c_str());
        if (!G.loaderNote.empty()) ImGui::TextColored(G.loaderName.empty() ? kBad : kWarn, "%s", G.loaderNote.c_str());
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(!canPlay);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.07f, 0.05f, 1));
    ImGui::PushFont(g_mid);
    if (ImGui::Button("PLAY CO-OP", ImVec2(260, 52))) Play();
    ImGui::PopFont(); ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (!g_msg.empty()) ImGui::TextColored(g_msgOk ? kOk : kBad, "%s", g_msg.c_str());
    ImGui::Spacing(); ImGui::Separator();
    ImGui::TextColored(kDim, "Starting the game from Steam or Ubisoft Connect runs the normal game, without co-op.");
    ImGui::TextColored(kDim, "Steam and Ubisoft Connect must be running. Mod protocol %u.", (unsigned)NET_PROTO_VER);
}

void ParseArgs() {
    int argc = 0; LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &argc);
    auto s = [&](int i) { char b[512]; WideCharToMultiByte(CP_UTF8, 0, w[i], -1, b, sizeof(b), nullptr, nullptr); return std::string(b); };
    for (int i = 1; i < argc; ++i) {
        std::string a = s(i); auto next = [&]() { return i + 1 < argc ? s(++i) : std::string(); };
        if (a == "--game-dir") A.gameDir = next(); else if (a == "--report") A.report = next(); else if (a == "--shot") A.shot = next();
        else if (a == "--payload") A.payload = next(); else if (a == "--exit-after") A.exitAfter = atoi(next().c_str());
        else if (a == "--no-launch") A.noLaunch = true; else if (a == "--auto-play") A.autoPlay = true;
    }
    LocalFree(w);
}
void Report() {
    if (A.report.empty()) return;
    FILE* f = fopen(A.report.c_str(), "w"); if (!f) return;
    fprintf(f, "gameDir=%s\nfound=%d\nknown=%d\npayloadOk=%d\nupToDate=%d\nloader=%s\nloaderNote=%s\nmsg=%s\nmsgOk=%d\nlaunched=%d\n",
            g_gameDir, G.found, G.known, G.payloadOk, G.upToDate, G.loaderName.c_str(), G.loaderNote.c_str(), g_msg.c_str(), g_msgOk, g_launched);
    fclose(f);
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    ParseArgs(); LoadSettings();
    if (!A.gameDir.empty()) lstrcpynA(g_gameDir, A.gameDir.c_str(), MAX_PATH);
    if (!g_gameDir[0]) FindGame();
    CheckGame();
    if (A.autoPlay) Play();

    WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst, LoadIcon(nullptr, IDI_APPLICATION), LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"AC4CoopLauncher", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"AC4 Co-op", WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME), 100, 100, 640, 520, nullptr, nullptr, inst, nullptr);
    if (!CreateDevice(hwnd)) { MessageBoxA(nullptr, "Direct3D 11 is not available on this PC.", "AC4 Co-op", MB_ICONERROR); return 1; }
    ShowWindow(hwnd, SW_SHOWDEFAULT); UpdateWindow(hwnd);
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); io.IniFilename = nullptr;
    char font[MAX_PATH]; GetWindowsDirectoryA(font, MAX_PATH); lstrcatA(font, "\\Fonts\\segoeui.ttf");
    const std::string lato = Join(PayloadDir(), "coop_ui\\LatoLatin-Regular.ttf"), latoB = Join(PayloadDir(), "coop_ui\\LatoLatin-Bold.ttf");
    if (FileExists(lato)) io.Fonts->AddFontFromFileTTF(lato.c_str(), 18.0f); else if (FileExists(font)) io.Fonts->AddFontFromFileTTF(font, 18.0f); else io.Fonts->AddFontDefault();
    if (FileExists(latoB)) g_big = io.Fonts->AddFontFromFileTTF(latoB.c_str(), 40.0f); else if (FileExists(font)) g_big = io.Fonts->AddFontFromFileTTF(font, 40.0f);
    if (!g_big) { ImFontConfig fc; fc.SizePixels = 32; g_big = io.Fonts->AddFontDefault(&fc); }
    if (FileExists(latoB)) g_mid = io.Fonts->AddFontFromFileTTF(latoB.c_str(), 22.0f);
    if (!g_mid) g_mid = io.Fonts->Fonts[0];
    Style();
    ImGui_ImplWin32_Init(hwnd); ImGui_ImplDX11_Init(g_dev, g_ctx);

    const ULONGLONG t0 = GetTickCount64();
    bool shotDone = false, done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); if (msg.message == WM_QUIT) done = true; }
        if (done) break;
        if (g_resizeW) { CleanupRTV(); g_sc->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0); g_resizeW = g_resizeH = 0; CreateRTV(); }
        static ULONGLONG lastReport = 0;
        if (!A.report.empty() && GetTickCount64() - lastReport > 1000) { lastReport = GetTickCount64(); Report(); }
        if (A.exitAfter && GetTickCount64() - t0 > (ULONGLONG)A.exitAfter * 1000) break;

        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        Ui();
        ImGui::End();
        ImGui::Render();
        const float clear[4] = { 0.08f, 0.07f, 0.05f, 1 };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr); g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_sc->Present(1, 0);
        if (!A.shot.empty() && !shotDone && GetTickCount64() - t0 > 3000) { shotDone = true; SaveShot(A.shot.c_str()); }
    }
    Report();
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    CleanupRTV(); if (g_sc) g_sc->Release(); if (g_ctx) g_ctx->Release(); if (g_dev) g_dev->Release();
    DestroyWindow(hwnd); UnregisterClassW(wc.lpszClassName, inst);
    return 0;
}
