// AC4Coop Launcher: find the game, check its build, install the mod, gather a party, start everyone's game.
// docs/design/launcher.md. Dear ImGui (third_party/imgui, v1.91.5) on Win32 + D3D11 in its own window - no game
// hooking. The game is started as AC4BFSP.exe from its folder (Steam and Ubisoft Connect ship the same exe).
//
// Test flags (for unattended checks; normal users start it with no arguments):
//   --game-dir DIR  --no-launch  --port N  --name NAME  --relay HOST[:PORT]  --no-upnp  --no-stun
//   --auto-host  --auto-join CODE  --auto-start N (host: Start once N members)  --exit-after S
//   --auto-install  --report FILE (state as key=value, refreshed every second)  --shot FILE.bmp (window image after 4 s)  --payload DIR
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <bcrypt.h>
#include <d3d11.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include "party_net.h"
#include "firewall.h"
#include "upnp.h"       // VpnLikeAdapters (hint line)
#include "netstate.h"   // NET_PROTO_VER only (header-only constants)

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

// ---------------------------------------------------------------- settings + args
struct Args {
    std::string gameDir, name, relay, autoJoin, report, shot, payload, fwApply, fwStatus;
    int port = 0, autoStart = 0, exitAfter = 0, cycles = 1;
    bool noLaunch = false, autoHost = false, noUpnp = false, noStun = false, autoInstall = false;
} A;

char g_exeDir[MAX_PATH], g_settings[MAX_PATH];
char g_gameDir[MAX_PATH] = {0}, g_name[Party::kNameLen] = "Player", g_relay[128] = {0}, g_joinCode[32] = {0};
int  g_port = 47801;

void LoadSettings() {
    GetModuleFileNameA(nullptr, g_exeDir, MAX_PATH); *strrchr(g_exeDir, '\\') = 0;
    _snprintf(g_settings, MAX_PATH, "%s\\launcher.ini", g_exeDir);
    GetPrivateProfileStringA("Launcher", "GameDir", "", g_gameDir, MAX_PATH, g_settings);
    GetPrivateProfileStringA("Launcher", "Name", "", g_name, sizeof(g_name), g_settings);
    if (!g_name[0]) { DWORD n = sizeof(g_name); if (!GetUserNameA(g_name, &n)) lstrcpyA(g_name, "Player"); g_name[Party::kNameLen - 1] = 0; }
    GetPrivateProfileStringA("Launcher", "RelayServer", "", g_relay, sizeof(g_relay), g_settings);
    g_port = GetPrivateProfileIntA("Launcher", "Port", 47801, g_settings);
}
// launcher_log.txt next to the exe: what the launcher did on Start / Install, for diagnosing a party remotely.
void LogLine(const char* fmt, ...) {
    char path[MAX_PATH]; _snprintf(path, MAX_PATH, "%s\\launcher_log.txt", g_exeDir);
    FILE* f = fopen(path, "a"); if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fputc('\n', f); fclose(f);
}
void SaveSettings() {
    if (!A.gameDir.empty()) return;   // test runs never overwrite the user's launcher.ini
    WritePrivateProfileStringA("Launcher", "GameDir", g_gameDir, g_settings);
    WritePrivateProfileStringA("Launcher", "Name", g_name, g_settings);
    WritePrivateProfileStringA("Launcher", "RelayServer", g_relay, g_settings);
}

// ---------------------------------------------------------------- game folder, hashes, install
bool FileExists(const std::string& p) { DWORD a = GetFileAttributesA(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
std::string Join(const char* dir, const char* leaf) { return std::string(dir) + "\\" + leaf; }

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

std::string PayloadDir() { return A.payload.empty() ? Join(g_exeDir, "files") : A.payload; }

// known_builds.txt: "<sha256> <label>" lines; '#' comments.
bool KnownBuild(const char* sha, std::string* label) {
    FILE* f = fopen(Join(PayloadDir().c_str(), "known_builds.txt").c_str(), "r");
    if (!f) return false;
    char line[512]; bool ok = false;
    while (!ok && fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || strlen(line) < 64) continue;
        if (_strnicmp(line, sha, 64) == 0) { ok = true; if (label) { *label = line + 65; while (!label->empty() && (label->back() == '\n' || label->back() == '\r')) label->pop_back(); } }
    }
    fclose(f);
    return ok;
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

struct GameInfo { bool found = false, known = false, modInstalled = false, modCurrent = false; std::string label, exeSha, asiSha, payloadAsiSha;
                  Firewall::Status fw = Firewall::UNKNOWN; std::string fwText, vpn; } G;
void CheckFirewall() {
    char d[200] = {0};
    G.fw = G.found ? Firewall::Query(Join(g_gameDir, "AC4BFSP.exe").c_str(), d, sizeof(d)) : Firewall::UNKNOWN;
    G.fwText = d;
}
void CheckGame() {
    G = GameInfo{};
    const std::string exe = Join(g_gameDir, "AC4BFSP.exe");
    G.found = g_gameDir[0] && FileExists(exe);
    CheckFirewall();
    { char v[256] = {0}; if (Upnp::VpnLikeAdapters(v, sizeof(v)) > 0) G.vpn = v; }
    char hex[65];
    if (G.found && Sha256File(exe, hex)) { G.exeSha = hex; G.known = KnownBuild(hex, &G.label); }
    if (Sha256File(Join(PayloadDir().c_str(), "AC4Coop.asi"), hex)) G.payloadAsiSha = hex;
    if (G.found && Sha256File(Join(g_gameDir, "AC4Coop.asi"), hex)) { G.modInstalled = true; G.asiSha = hex; G.modCurrent = G.asiSha == G.payloadAsiSha; }
}

// Unattended test runs (--exit-after / --shot / --report) must never change a game folder or the firewall from a
// stray click: on 2026-11-12 a screenshot run pointed at the real game folder reinstalled an older mod file when
// a click landed on the button as the window appeared. In a test run only --auto-install may install.
bool TestRun() { return A.exitAfter || !A.shot.empty() || !A.report.empty(); }

std::string g_installMsg;
bool Install() {
    if (TestRun() && !A.autoInstall) { g_installMsg = "Test run: install is disabled"; LogLine("install refused: test run"); return false; }
    const char* files[] = { "AC4Coop.asi", "dinput8.dll", "known_builds.txt" };
    for (const char* f : files) {
        const std::string src = Join(PayloadDir().c_str(), f), dst = Join(g_gameDir, f);
        if (!FileExists(src)) { g_installMsg = std::string("Missing in the launcher's files folder: ") + f; return false; }
        if (FileExists(dst)) CopyFileA(dst.c_str(), (dst + ".launcher-bak").c_str(), FALSE);
        if (!CopyFileA(src.c_str(), dst.c_str(), FALSE)) { g_installMsg = std::string("Could not write ") + dst + " (is the game running?)"; return false; }
    }
    const std::string ini = Join(g_gameDir, "AC4Coop.ini"), tpl = Join(PayloadDir().c_str(), "AC4Coop.ini");
    if (!FileExists(ini) && FileExists(tpl)) CopyFileA(tpl.c_str(), ini.c_str(), TRUE);   // never overwrite the player's ini
    g_installMsg = "Installed."; CheckGame();
    LogLine("installed into %s (asi %.8s)", g_gameDir, G.asiSha.c_str());
    return true;
}

// ---------------------------------------------------------------- start: write the game's ini, launch it
std::string g_launchMsg;
bool g_launched = false;
std::string g_iniWritten;
bool WriteGameIni(const PartyNet::State& s) {
    // A test run writes only into a folder named on its command line, never into the player's saved game folder.
    if (TestRun() && A.gameDir.empty()) { g_launchMsg = "Test run without --game-dir: nothing written"; return false; }
    const std::string ini = Join(g_gameDir, "AC4Coop.ini");
    if (!FileExists(ini)) { g_launchMsg = "AC4Coop.ini is missing - click Install first"; return false; }
    CopyFileA(ini.c_str(), (ini + ".launcher.bak").c_str(), FALSE);
    char room[24] = "";
    if (s.isHost && s.start.relayRoom) _snprintf(room, sizeof(room), "%010llx", (unsigned long long)s.start.relayRoom);
    char port[16]; _snprintf(port, sizeof(port), "%d", g_port);
    const char* kv[][2] = {
        { "Enabled", "1" }, { "Mode", s.isHost ? "host" : "join" }, { "JoinCode", s.isHost ? "" : s.start.joinCode },
        { "RelayServer", s.start.relayServer }, { "RelayRoom", room }, { "LocalPort", port },
    };
    g_iniWritten.clear();
    for (auto& p : kv) { WritePrivateProfileStringA("Net", p[0], p[1], ini.c_str()); g_iniWritten += std::string(p[0]) + "=" + p[1] + " "; }
    // Co-op sync profile (test-ready build, user's request 2026-11-13). The role decides who owns the world; every
    // sync feature listed here has run live on the replay rig with 0 faults (builds D219B303 .. 5F58215C,
    // docs/evidence/action-writers-2026-11-13.md and the run notes in the commits). GuardActionGate (run 24F7CCBC:
    // 5271 AI switches blocked, 0 faults) and FootOffsetFix (run 9E2D2FF1: 0 faults) added after their own runs.
    // GuardSeesPuppet stays out until its host-side run. The plugin's code defaults stay 0; this is the only place
    // that turns them on.
    const char* coop[][2] = {
        { "EntityHost", s.isHost ? "1" : "0" }, { "EntitySync", "1" }, { "EntityFrameCadence", "1" },
        { "OwnUpdateGuards", "1" }, { "OwnUpdatePuppet", "1" }, { "OwnPhysicsMs", "1" },
        { "MirrorActions", "1" }, { "MirrorActionsDeferred", "1" }, { "GuardStaleReturn", "1" },
        { "GuardAnimFull", "1" }, { "GuardBlendFix", "1" }, { "BlendTypeSafe", "1" },
        { "GuardPhaseLock", "1" }, { "GuardInterpBuffer", "1" }, { "GuardInterpDelayMs", "120" },
        { "PuppetHostTime", "1" }, { "PuppetActionReassert", "1" }, { "BindRadiusM", "60" }, { "CrashContext", "1" },
        { "GuardActionGate", "1" }, { "FootOffsetFix", "1" },
    };
    for (auto& p : coop) { WritePrivateProfileStringA("Coop", p[0], p[1], ini.c_str()); g_iniWritten += std::string(p[0]) + "=" + p[1] + " "; }
    return true;
}
bool LaunchGame() {
    if (A.noLaunch) { g_launchMsg = "Test mode: ini written, game not launched"; return true; }
    STARTUPINFOA si{ sizeof(si) }; PROCESS_INFORMATION pi{};
    std::string exe = Join(g_gameDir, "AC4BFSP.exe");
    if (!CreateProcessA(exe.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, g_gameDir, &si, &pi)) {
        char b[128]; _snprintf(b, sizeof(b), "Could not start the game (error %lu)", GetLastError()); g_launchMsg = b; return false;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    g_launchMsg = "Game started - this window can be closed.";
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
    s.WindowRounding = 0; s.FrameRounding = 4; s.GrabRounding = 4; s.FramePadding = ImVec2(10, 6); s.ItemSpacing = ImVec2(10, 8);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.10f, 0.12f, 1); c[ImGuiCol_Button] = ImVec4(0.16f, 0.36f, 0.55f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.45f, 0.68f, 1); c[ImGuiCol_ButtonActive] = ImVec4(0.13f, 0.30f, 0.47f, 1);
    c[ImGuiCol_Header] = ImVec4(0.16f, 0.36f, 0.55f, 0.6f);
}
ImFont* g_big = nullptr;
const ImVec4 kOk(0.45f, 0.85f, 0.45f, 1), kWarn(0.95f, 0.75f, 0.3f, 1), kBad(0.95f, 0.4f, 0.4f, 1), kDim(0.6f, 0.62f, 0.66f, 1);

void UiGame() {
    ImGui::SeparatorText("Game");
    ImGui::TextColored(kDim, "Folder"); ImGui::SameLine(90);
    ImGui::TextWrapped("%s", g_gameDir[0] ? g_gameDir : "(not found)");
    if (ImGui::Button("Find")) { if (!FindGame()) g_installMsg = "Not found in Steam or Ubisoft Connect libraries - use Browse"; SaveSettings(); CheckGame(); }
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        char file[MAX_PATH] = "AC4BFSP.exe"; OPENFILENAMEA o{ sizeof(o) }; o.lpstrFilter = "AC4BFSP.exe\0AC4BFSP.exe\0"; o.lpstrFile = file; o.nMaxFile = MAX_PATH; o.Flags = OFN_FILEMUSTEXIST;
        if (GetOpenFileNameA(&o)) { *strrchr(file, '\\') = 0; lstrcpynA(g_gameDir, file, MAX_PATH); SaveSettings(); CheckGame(); }
    }
    if (!G.found) { ImGui::TextColored(kBad, "AC4BFSP.exe not found"); return; }
    if (G.known) ImGui::TextColored(kOk, "Supported game build (%s)", G.label.c_str());
    else ImGui::TextColored(kBad, "Unsupported game build (sha256 %.12s...) - the mod will stay inactive", G.exeSha.c_str());
    if (!G.modInstalled) ImGui::TextColored(kWarn, "Mod not installed");
    else if (G.modCurrent) ImGui::TextColored(kOk, "Mod installed and up to date (%.8s)", G.asiSha.c_str());
    else ImGui::TextColored(kWarn, "Mod installed but different from this launcher's build (%.8s vs %.8s)", G.asiSha.c_str(), G.payloadAsiSha.c_str());
    if (ImGui::Button(G.modInstalled ? "Update / repair mod" : "Install mod")) Install();
    if (!g_installMsg.empty()) { ImGui::SameLine(); ImGui::TextColored(kDim, "%s", g_installMsg.c_str()); }
    // Windows Firewall: a host must receive an unsolicited packet. Read-only status; the fix is one admin prompt.
    static std::string fwMsg;
    if (G.fw != Firewall::UNKNOWN) {
        const bool good = G.fw == Firewall::ALLOWED || G.fw == Firewall::FIREWALL_OFF;
        ImGui::TextColored(good ? kOk : G.fw == Firewall::BLOCKED ? kBad : kWarn, "%s", G.fwText.c_str());
        if (!good) {
            if (ImGui::Button("Allow the game through the firewall (admin prompt)")) {
                char m[200] = {0};
                if (TestRun()) lstrcpyA(m, "Test run: firewall change is disabled");
                else Firewall::RunElevated(Join(g_gameDir, "AC4BFSP.exe").c_str(), m, sizeof(m));
                fwMsg = m; LogLine("firewall: %s", m); CheckFirewall();
            }
        }
        if (!fwMsg.empty()) { ImGui::SameLine(); ImGui::TextColored(kDim, "%s", fwMsg.c_str()); }
    }
    if (!G.vpn.empty())
        ImGui::TextColored(kWarn, "VPN-type adapter active: %s. If the game cannot connect, disconnect the VPN (or its kill switch / split tunnelling).", G.vpn.c_str());
}

void UiParty(const PartyNet::State& s) {
    ImGui::SeparatorText("Party");
    const bool idle = s.phase == PartyNet::IDLE || s.phase == PartyNet::FAILED;
    ImGui::BeginDisabled(!idle);
    ImGui::SetNextItemWidth(220); if (ImGui::InputText("Your name", g_name, sizeof(g_name))) SaveSettings();
    ImGui::SetNextItemWidth(220); if (ImGui::InputTextWithHint("Relay server (optional)", "host:47800", g_relay, sizeof(g_relay))) SaveSettings();
    ImGui::EndDisabled();
    if (!s.portIsGamePort) ImGui::TextColored(kWarn, "Game port %d is busy (game running?) - using port %d; codes made now will not carry over to the game", g_port, s.boundPort);
    ImGui::Spacing();
    if (idle) {
        if (ImGui::Button("Host a party", ImVec2(200, 0))) { PartyNet::Config c; c.port = g_port; lstrcpynA(c.name, g_name, sizeof(c.name)); lstrcpynA(c.relay, g_relay, sizeof(c.relay)); c.gameProto = NET_PROTO_VER; c.upnp = !A.noUpnp; if (A.noStun) c.stunServer[0] = 0; char e[128]; PartyNet::Init(c, e, sizeof(e)); PartyNet::Host(); }
        ImGui::SameLine(); ImGui::TextColored(kDim, "or join:"); ImGui::SameLine();
        ImGui::SetNextItemWidth(190);
        const bool enter = ImGui::InputTextWithHint("##code", "XXXX-XXXX-XXXX", g_joinCode, sizeof(g_joinCode), ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        static std::string joinErr;
        if (ImGui::Button("Join") || enter) {
            PartyNet::Config c; c.port = g_port; lstrcpynA(c.name, g_name, sizeof(c.name)); lstrcpynA(c.relay, g_relay, sizeof(c.relay)); c.gameProto = NET_PROTO_VER; c.upnp = false;
            char e[160] = {0}; PartyNet::Init(c, e, sizeof(e));
            joinErr = PartyNet::Join(g_joinCode, e, sizeof(e)) ? "" : e;
        }
        if (!joinErr.empty()) ImGui::TextColored(kBad, "%s", joinErr.c_str());
    }
    if (s.isHost && s.phase != PartyNet::IDLE) {
        ImGui::TextColored(kDim, "Your party code");
        ImGui::PushFont(g_big); ImGui::Text("%s", s.code[0] ? s.code : "..."); ImGui::PopFont();
        if (s.code[0]) { ImGui::SameLine(); if (ImGui::Button("Copy")) ImGui::SetClipboardText(s.code); }
        if (s.lanCode[0]) { ImGui::TextColored(kDim, "Same network (LAN):"); ImGui::SameLine(); ImGui::Text("%s", s.lanCode); ImGui::SameLine(); if (ImGui::SmallButton("Copy LAN code")) ImGui::SetClipboardText(s.lanCode); }
        if (s.verdict[0]) ImGui::TextColored(!strcmp(s.verdict, "OK") || !strcmp(s.verdict, "PUBLIC") || !strcmp(s.verdict, "RELAY") ? kOk : kWarn, "%s", s.advice);
    } else if (!s.isHost && s.phase != PartyNet::IDLE) {
        ImGui::TextColored(kDim, "Party"); ImGui::SameLine(); ImGui::Text("%s", s.code);
    }
    if (s.count > 0) {
        ImGui::Spacing(); ImGui::TextColored(kDim, "Players (%d) - the game supports %d for now", s.count, s.maxPlay);
        for (int i = 0; i < s.count; ++i) {
            const bool plays = i < s.maxPlay;
            ImGui::BulletText("%s%s%s", s.members[i].name, s.members[i].slot == 0 ? "  (host)" : "", plays ? "" : "  - waiting (game is 2-player for now)");
        }
    }
    ImGui::Spacing();
    ImGui::TextWrapped("%s", s.status);
    if (s.isHost && s.phase == PartyNet::HOSTING) {
        ImGui::BeginDisabled(!G.found || !G.modInstalled);
        if (ImGui::Button("Start game for everyone", ImVec2(260, 36))) PartyNet::StartGame();
        ImGui::EndDisabled();
        if (!G.modInstalled) { ImGui::SameLine(); ImGui::TextColored(kWarn, "Install the mod first"); }
    }
    if (!idle) { ImGui::SameLine(); if (ImGui::Button(s.isHost ? "Close party" : "Leave")) PartyNet::Leave(); }
    if (!g_launchMsg.empty()) ImGui::TextColored(g_launched ? kOk : kBad, "%s", g_launchMsg.c_str());
    if (!g_iniWritten.empty()) ImGui::TextColored(kDim, "AC4Coop.ini [Net]: %s", g_iniWritten.c_str());
}

void ParseArgs() {
    int argc = 0; LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &argc);
    auto s = [&](int i) { char b[512]; WideCharToMultiByte(CP_UTF8, 0, w[i], -1, b, sizeof(b), nullptr, nullptr); return std::string(b); };
    for (int i = 1; i < argc; ++i) {
        std::string a = s(i); auto next = [&]() { return i + 1 < argc ? s(++i) : std::string(); };
        if (a == "--game-dir") A.gameDir = next(); else if (a == "--name") A.name = next(); else if (a == "--relay") A.relay = next();
        else if (a == "--auto-join") A.autoJoin = next(); else if (a == "--report") A.report = next(); else if (a == "--shot") A.shot = next();
        else if (a == "--payload") A.payload = next(); else if (a == "--port") A.port = atoi(next().c_str());
        else if (a == "--auto-start") A.autoStart = atoi(next().c_str()); else if (a == "--cycles") A.cycles = atoi(next().c_str()); else if (a == "--exit-after") A.exitAfter = atoi(next().c_str());
        else if (a == "--no-launch") A.noLaunch = true; else if (a == "--auto-host") A.autoHost = true;
        else if (a == "--no-upnp") A.noUpnp = true; else if (a == "--no-stun") A.noStun = true;
        else if (a == "--auto-install") A.autoInstall = true;
        else if (a == "--firewall-apply") A.fwApply = next();     // elevated helper mode (started by RunElevated)
        else if (a == "--firewall-status") A.fwStatus = next();   // read-only: log the status and exit
    }
    LocalFree(w);
}

void Report(const PartyNet::State& s) {
    if (A.report.empty()) return;
    FILE* f = fopen(A.report.c_str(), "w"); if (!f) return;
    fprintf(f, "lanCode=%s\n", s.lanCode); fprintf(f, "phase=%d\nisHost=%d\ncode=%s\nverdict=%s\ncount=%d\nstatus=%s\nlaunched=%d\nlaunchMsg=%s\niniWritten=%s\nstartJoinCode=%s\nstartRelayRoom=%010llx\n",
            (int)s.phase, s.isHost ? 1 : 0, s.code, s.verdict, s.count, s.status, g_launched ? 1 : 0, g_launchMsg.c_str(), g_iniWritten.c_str(),
            s.start.joinCode, (unsigned long long)s.start.relayRoom);
    for (int i = 0; i < s.count; ++i) fprintf(f, "member%d=%s slot=%u\n", i, s.members[i].name, s.members[i].slot);
    fclose(f);
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    ParseArgs(); LoadSettings();
    // Helper modes: no window. --firewall-apply runs elevated (one UAC prompt from the button); --firewall-status
    // is read-only.
    if (!A.fwApply.empty()) {
        char m[256] = {0}; const bool ok = Firewall::Apply(A.fwApply.c_str(), m, sizeof(m));
        LogLine("firewall-apply \"%s\": %s", A.fwApply.c_str(), m);
        return ok ? 0 : 1;
    }
    if (!A.fwStatus.empty()) {
        char m[256] = {0}, v[256] = {0}; const Firewall::Status st = Firewall::Query(A.fwStatus.c_str(), m, sizeof(m));
        Upnp::VpnLikeAdapters(v, sizeof(v));
        LogLine("firewall-status \"%s\": %d %s | vpn-type adapters: %s", A.fwStatus.c_str(), (int)st, m, v[0] ? v : "none");
        return 0;
    }
    if (!A.gameDir.empty()) lstrcpynA(g_gameDir, A.gameDir.c_str(), MAX_PATH);
    if (!A.name.empty()) lstrcpynA(g_name, A.name.c_str(), sizeof(g_name));
    if (!A.relay.empty()) lstrcpynA(g_relay, A.relay.c_str(), sizeof(g_relay));
    if (A.port) g_port = A.port;
    if (!g_gameDir[0]) FindGame();
    CheckGame();
    if (A.autoInstall && G.found) Install();

    WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst, LoadIcon(nullptr, IDI_APPLICATION), LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"AC4CoopLauncher", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"AC4 Co-op Launcher", WS_OVERLAPPEDWINDOW, 100, 100, 760, 640, nullptr, nullptr, inst, nullptr);
    if (!CreateDevice(hwnd)) { MessageBoxA(nullptr, "Direct3D 11 is not available on this PC.", "AC4 Co-op Launcher", MB_ICONERROR); return 1; }
    ShowWindow(hwnd, SW_SHOWDEFAULT); UpdateWindow(hwnd);
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); io.IniFilename = nullptr;
    char font[MAX_PATH]; GetWindowsDirectoryA(font, MAX_PATH); lstrcatA(font, "\\Fonts\\segoeui.ttf");
    if (FileExists(font)) { io.Fonts->AddFontFromFileTTF(font, 18.0f); g_big = io.Fonts->AddFontFromFileTTF(font, 40.0f); }
    if (!g_big) { io.Fonts->AddFontDefault(); ImFontConfig fc; fc.SizePixels = 32; g_big = io.Fonts->AddFontDefault(&fc); }
    Style();
    ImGui_ImplWin32_Init(hwnd); ImGui_ImplDX11_Init(g_dev, g_ctx);

    const ULONGLONG t0 = GetTickCount64();
    bool autoDone = false, shotDone = false, startHandled = false;
    ULONGLONG startedAt = 0;
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); if (msg.message == WM_QUIT) done = true; }
        if (done) break;
        if (g_resizeW) { CleanupRTV(); g_sc->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0); g_resizeW = g_resizeH = 0; CreateRTV(); }
        const PartyNet::State s = PartyNet::Snapshot();

        // Unattended test actions.
        if (!autoDone && (A.autoHost || !A.autoJoin.empty())) {
            autoDone = true;
            PartyNet::Config c; c.port = g_port; lstrcpynA(c.name, g_name, sizeof(c.name)); lstrcpynA(c.relay, g_relay, sizeof(c.relay)); c.gameProto = NET_PROTO_VER;
            c.upnp = !A.noUpnp && A.autoHost; if (A.noStun) c.stunServer[0] = 0;
            char e[160] = {0}; PartyNet::Init(c, e, sizeof(e));
            if (A.autoHost) PartyNet::Host(); else { lstrcpynA(g_joinCode, A.autoJoin.c_str(), sizeof(g_joinCode)); PartyNet::Join(g_joinCode, e, sizeof(e)); }
        }
        if (A.autoStart && s.isHost && s.phase == PartyNet::HOSTING && s.count >= A.autoStart) PartyNet::StartGame();

        // START (host clicked it, or it arrived from the host): configure and launch this game, once per party.
        // Re-armed whenever the party leaves STARTED (Leave / Close party / a new Host or Join): it used to be
        // once per launcher run, so a second party in the same window sent START to the guests but never wrote
        // this game's ini or launched it (found in the first in-game test, 2026-11-12).
        if (s.phase != PartyNet::STARTED && startHandled) { startHandled = false; g_launched = false; g_launchMsg.clear(); g_iniWritten.clear(); }
        if (s.phase == PartyNet::STARTED && !startHandled) {
            startHandled = true;
            LogLine("START received (%s), code %s, game folder %s", s.isHost ? "host" : "guest", s.start.joinCode, g_gameDir);
            if (WriteGameIni(s)) {
                LogLine("ini written: %s", g_iniWritten.c_str());
                if (s.isHost) Sleep(800);       // let the START repeats go out before the port changes owner
                PartyNet::Shutdown();           // the game binds this port next
                g_launched = LaunchGame();
            }
            LogLine("launch: %s", g_launchMsg.c_str());
            startedAt = GetTickCount64();
        }
        // Test only (--cycles N): 3 s after a START was handled, leave and run the party again in this same window.
        if (startHandled && A.cycles > 1 && startedAt && GetTickCount64() - startedAt > 3000) {
            --A.cycles; startedAt = 0; PartyNet::Leave(); autoDone = false; LogLine("test: next party cycle (%d left)", A.cycles);
        }
        static ULONGLONG lastReport = 0;
        if (!A.report.empty() && GetTickCount64() - lastReport > 1000) { lastReport = GetTickCount64(); Report(s); }
        if (!A.shot.empty() && !shotDone && GetTickCount64() - t0 > 4000) { shotDone = true; SaveShot(A.shot.c_str()); }
        if (A.exitAfter && GetTickCount64() - t0 > (ULONGLONG)A.exitAfter * 1000) { Report(PartyNet::Snapshot()); break; }

        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PushFont(g_big); ImGui::Text("AC4 Co-op"); ImGui::PopFont();
        ImGui::SameLine(); ImGui::TextColored(kDim, "launcher - mod protocol %u", (unsigned)NET_PROTO_VER);
        UiGame();
        UiParty(PartyNet::Snapshot());
        ImGui::End();
        ImGui::Render();
        const float clear[4] = { 0.09f, 0.10f, 0.12f, 1 };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr); g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_sc->Present(1, 0);
    }
    Report(PartyNet::Snapshot());
    PartyNet::Leave(); PartyNet::Shutdown();
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    CleanupRTV(); if (g_sc) g_sc->Release(); if (g_ctx) g_ctx->Release(); if (g_dev) g_dev->Release();
    DestroyWindow(hwnd); UnregisterClassW(wc.lpszClassName, inst);
    return 0;
}
