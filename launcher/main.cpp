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
//   --ini FILE (use this AC4Coop.ini instead of <game>\AC4Coop.ini)  --set-role host|guest (one [Coop] EntityHost write)
//   --install-dry FILE (run only Install()'s ini-key step on FILE, then exit)
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>   // before windows.h (winsock.h clash); bind test, local IPv4s, UPnP probe
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
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
#include "upnp.h"       // src/upnp.cpp (Winsock + WinHTTP only, no engine calls): the self-check's UPnP probe
#include "stun.h"       // src/stun.cpp (Winsock only, no engine calls): the self-check's STUN probe

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "winhttp.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

// ---------------------------------------------------------------- settings + args
struct Args { std::string gameDir, report, shot, payload, ini, setRole, installDry; int exitAfter = 0; bool noLaunch = false, autoPlay = false; } A;
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

std::wstring Widen(const std::string& s);
bool ApplyInstallKeys(const std::string& iniA);
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
    if (!ApplyInstallKeys(ini)) { Msg(false, "Could not write " + ini); return false; }
    CheckGame();
    Msg(true, "Co-op mod installed (" + G.loaderName + " loader)");
    return true;
}

// The keys the co-op start needs; everything else in the player's ini is left alone. One key per
// WritePrivateProfileStringW (the file is never rewritten whole). [Net] Mode / Transport are the player's choice
// (Mode dropdown, eos vs udp): they are written only when ABSENT, so an install/update never resets them.
// (12d5abc wrote them on every install as first-install defaults; it was not a migration.)
bool IniKeyPresent(const std::wstring& ini, const wchar_t* sec, const wchar_t* key) {
    static const wchar_t kAbsent[] = L"\x01<absent>";   // a value no real ini line can carry
    wchar_t b[64];
    GetPrivateProfileStringW(sec, key, kAbsent, b, 64, ini.c_str());
    return wcscmp(b, kAbsent) != 0;
}
bool ApplyInstallKeys(const std::string& iniA) {
    const std::wstring ini = Widen(iniA);
    struct KV { const char* sec; const char* key; const char* val; bool onlyIfAbsent; };
    const KV kv[] = {
        { "Launcher", "RequireArm", "1" },        // plain Steam / Ubisoft starts stay unmodded
        { "Net", "Enabled", "1" }, { "Net", "Mode", "manual", true }, { "Net", "Transport", "eos", true }, { "Net", "EosAutoStart", "0" },
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
    bool ok = true;
    for (auto& p : kv) {
        const std::wstring sec = Widen(p.sec), key = Widen(p.key);
        if (p.onlyIfAbsent && IniKeyPresent(ini, sec.c_str(), key.c_str())) { LogLine("ini [%s] %s present - kept", p.sec, p.key); continue; }
        ok &= WritePrivateProfileStringW(sec.c_str(), key.c_str(), Widen(p.val).c_str(), ini.c_str()) != 0;
    }
    return ok;
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

// ---------------------------------------------------------------- co-op ini: role, mode, relay (plan Phase 5)
// Every write is ONE key through WritePrivateProfileStringW - the file is never rewritten whole (a WriteAllText once
// truncated the live ini to 4096 B mid-run). The first write of a launcher run backs it up to AC4Coop.ini.bak-launcher.
std::wstring Widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 1 ? n - 1 : 0, L'\0'); if (n > 1) MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, &w[0], n);
    return w;
}
std::string IniPathA() { return A.ini.empty() ? Join(g_gameDir, "AC4Coop.ini") : A.ini; }
bool g_iniBackedUp = false;
bool IniSet(const wchar_t* sec, const wchar_t* key, const std::wstring& val) {
    const std::wstring ini = Widen(IniPathA());
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) { Msg(false, "AC4Coop.ini not found - press Play once to install"); return false; }
    if (!g_iniBackedUp) g_iniBackedUp = CopyFileW(ini.c_str(), (ini + L".bak-launcher").c_str(), FALSE) != 0;
    if (!g_iniBackedUp) { Msg(false, "Could not back up AC4Coop.ini - nothing written"); return false; }
    const bool ok = WritePrivateProfileStringW(sec, key, val.c_str(), ini.c_str()) != 0;
    LogLine("ini [%ls] %ls=%ls -> %s", sec, key, val.c_str(), ok ? "ok" : "FAILED");
    if (!ok) Msg(false, "Could not write AC4Coop.ini");
    return ok;
}

// The keys and defaults the mod reads: src/net.cpp Net::Init ([Net] Enabled/LocalPort/PeerPort/PeerIP/Mode/Transport/
// RelayServer/RelayRoom) and src/phase1.cpp ([Coop] EntityHost 0, NetStrictPeer 1, HostileOnly 0).
struct NetCfg {
    bool present = false; int entityHost = 0, strictPeer = 1, hostileOnly = 0, localPort = 47801, peerPort = 47801;
    char mode[16] = "manual", transport[16] = "udp", peerIp[64] = "127.0.0.1", relay[128] = "", room[32] = "";
} N;
int g_netGen = 0;   // bumps on every reload so the edit boxes resync
void LoadNet() {
    N = NetCfg{}; ++g_netGen;
    const std::string ini = IniPathA(); N.present = FileExists(ini); if (!N.present) return;
    const char* p = ini.c_str();
    N.entityHost = GetPrivateProfileIntA("Coop", "EntityHost", 0, p);
    N.strictPeer = GetPrivateProfileIntA("Coop", "NetStrictPeer", 1, p);
    N.hostileOnly = GetPrivateProfileIntA("Coop", "HostileOnly", 0, p);
    N.localPort = GetPrivateProfileIntA("Net", "LocalPort", 47801, p);
    N.peerPort = GetPrivateProfileIntA("Net", "PeerPort", 47801, p);
    GetPrivateProfileStringA("Net", "Mode", "manual", N.mode, sizeof(N.mode), p);
    GetPrivateProfileStringA("Net", "Transport", "udp", N.transport, sizeof(N.transport), p);
    GetPrivateProfileStringA("Net", "PeerIP", "127.0.0.1", N.peerIp, sizeof(N.peerIp), p);
    GetPrivateProfileStringA("Net", "RelayServer", "", N.relay, sizeof(N.relay), p);
    GetPrivateProfileStringA("Net", "RelayRoom", "", N.room, sizeof(N.room), p);
}
// net.cpp accepts exactly these Mode values (anything else logs "unknown [Net] Mode" and runs manual).
const char* kModes[] = { "manual", "host", "join", "lobby" };
int ModeIndex() { for (int i = 0; i < 4; ++i) if (!_stricmp(N.mode, kModes[i])) return i; return -1; }
// PeerIP / ports matter only in manual mode over UDP: Transport=eos forces the peer to the local bridge, host/join/lobby
// take the peer from the HELLO / the code (net.cpp Init).
bool ManualUdp() { return _stricmp(N.transport, "eos") != 0 && ModeIndex() <= 0; }
bool ParseIPv4(const char* s, uint32_t* ip) { in_addr a{}; if (inet_pton(AF_INET, s, &a) != 1) return false; *ip = ntohl(a.s_addr); return true; }
bool IsLoopback(const char* s) { uint32_t ip = 0; return ParseIPv4(s, &ip) && (ip >> 24) == 127; }
std::string IpStr(uint32_t ip) { char b[20]; _snprintf(b, sizeof(b), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255); return b; }

std::vector<uint32_t> LocalIPv4s() {   // up adapters, loopback excluded, host order
    std::vector<uint32_t> out; ULONG n = 16384; std::vector<char> buf(n);
    const ULONG fl = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG r = GetAdaptersAddresses(AF_INET, fl, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &n);
    if (r == ERROR_BUFFER_OVERFLOW) { buf.resize(n); r = GetAdaptersAddresses(AF_INET, fl, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &n); }
    if (r != NO_ERROR) return out;
    for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            const uint32_t ip = ntohl(((sockaddr_in*)u->Address.lpSockaddr)->sin_addr.s_addr);
            if ((ip >> 24) != 127) out.push_back(ip);
        }
    }
    return out;
}

// Co-op ASIs in the folders Ultimate ASI Loader loads from (root, scripts\, plugins\ - UNVERIFIED list, from the
// loader's documentation, not measured here). More than one = two copies of the mod hooking the same game.
std::vector<std::string> CoopAsis() {
    std::vector<std::string> out;
    for (const char* sub : { "", "scripts\\", "plugins\\" }) {
        WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((std::string(g_gameDir) + "\\" + sub + "*.asi").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do { std::string lo = fd.cFileName; for (auto& c : lo) c = (char)tolower((unsigned char)c);
             if (lo.find("coop") != std::string::npos) out.push_back(std::string(sub) + fd.cFileName); }
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return out;
}

// Preflight (backlog I-801/I-802, ops/PREFLIGHT-TWO-MACHINE-2026-11-09.md B1-B4): each of these fails silently in game.
struct Line { int sev; std::string text; };   // 0 ok, 1 warn, 2 fail
std::vector<Line> g_pre, g_check;
std::string Fmt(const char* fmt, ...) { char b[512]; va_list ap; va_start(ap, fmt); _vsnprintf(b, sizeof(b), fmt, ap); va_end(ap); b[sizeof(b) - 1] = 0; return b; }
void Preflight() {
    g_pre.clear(); if (!N.present) return;
    auto add = [](int s, const std::string& t) { g_pre.push_back({ s, t }); };
    if (G.found) {
        const std::string dep = Join(g_gameDir, "AC4Coop.asi"), pay = Join(PayloadDir(), "AC4Coop.asi");
        if (FileExists(dep) && FileExists(pay) && !SameFile(dep, pay))
            add(2, "B1 FAIL installed AC4Coop.asi is not this launcher's build - press Play to update; both PCs need the same build");
        const auto asis = CoopAsis();
        if (asis.size() > 1) { std::string l; for (auto& a : asis) l += (l.empty() ? "" : ", ") + a;
            add(2, "ASI2 FAIL " + std::to_string(asis.size()) + " co-op ASIs in the game folder (" + l + ") - keep only AC4Coop.asi"); }
    }
    if (ModeIndex() < 0) add(1, Fmt("MODE [Net] Mode=%s is not manual/host/join/lobby - the mod falls back to manual", N.mode));
    const bool manual = ManualUdp(), loop = IsLoopback(N.peerIp);
    if (manual && loop && N.strictPeer)
        add(2, "B2 FAIL PeerIP=127.0.0.1 with NetStrictPeer=1: packets from another PC are dropped - set PeerIP to the other PC (OK only for a same-PC two-instance test)");
    if (manual && !loop && N.localPort != N.peerPort)
        add(2, Fmt("B3 FAIL LocalPort=%d != PeerPort=%d with a remote PeerIP: the same ini on both PCs gives a one-way link - use one port on both", N.localPort, N.peerPort));
    if (N.hostileOnly) add(2, "B4 FAIL [Coop] HostileOnly=1 gates sync on a predicate never observed in game - set HostileOnly=0");
    if (manual && !loop) {
        uint32_t peer = 0; const auto ips = LocalIPv4s(); bool same = false; std::string l;
        if (!ParseIPv4(N.peerIp, &peer)) add(2, Fmt("NET FAIL PeerIP=%s is not an IPv4 address", N.peerIp));
        else {
            for (uint32_t ip : ips) { same |= (ip >> 8) == (peer >> 8); l += (l.empty() ? "" : ", ") + IpStr(ip); }
            if (!same) add(2, Fmt("NET FAIL PeerIP=%s is not on the same /24 as this PC (%s) - a LAN peer is unreachable; check the other PC's address", N.peerIp, l.empty() ? "no IPv4" : l.c_str()));
        }
    }
    bool bad = false; for (auto& p : g_pre) bad |= p.sev == 2;
    if (!bad) add(0, "Preflight: no known two-machine blocker in AC4Coop.ini");
}
void RefreshNet() { LoadNet(); Preflight(); }

// ---------------------------------------------------------------- firewall (elevated, after an explicit confirmation)
std::string FwName() { return "AC4Coop UDP " + std::to_string(N.localPort); }
std::string FwAddArgs() {
    return "advfirewall firewall add rule name=\"" + FwName() + "\" dir=in action=allow protocol=UDP localport=" +
           std::to_string(N.localPort) + " program=\"" + Join(g_gameDir, "AC4BFSP.exe") + "\" enable=yes";
}
DWORD RunNetsh(const std::string& args) {   // not elevated, hidden; exit code (or -1)
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring exe = std::wstring(sys) + L"\\netsh.exe"; std::wstring cl = L"\"" + exe + L"\" " + Widen(args);
    STARTUPINFOW si{ sizeof(si) }; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE; PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), &cl[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return (DWORD)-1;
    WaitForSingleObject(pi.hProcess, 15000); DWORD code = (DWORD)-1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return code;
}
bool FwRuleExists() { return RunNetsh("advfirewall firewall show rule name=\"" + FwName() + "\"") == 0; }
void AddFirewallRule() {
    if (!G.found) { Msg(false, "Find the game folder first"); return; }
    if (FwRuleExists()) { Msg(true, "Firewall rule \"" + FwName() + "\" already exists - nothing to do"); return; }
    const std::string cmd = "netsh " + FwAddArgs();
    const std::wstring q = L"Windows will ask for administrator rights to run exactly this command:\n\n" + Widen(cmd) + L"\n\nCreate the rule?";
    if (MessageBoxW(GetActiveWindow(), q.c_str(), L"AC4 Co-op - firewall", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) { Msg(false, "Firewall rule not created (cancelled)"); return; }
    const std::wstring args = Widen(FwAddArgs());
    SHELLEXECUTEINFOW se{ sizeof(se) }; se.fMask = SEE_MASK_NOCLOSEPROCESS; se.lpVerb = L"runas"; se.lpFile = L"netsh.exe";
    se.lpParameters = args.c_str(); se.nShow = SW_HIDE;
    if (!ShellExecuteExW(&se) || !se.hProcess) { Msg(false, "Firewall rule not created (administrator rights refused)"); return; }
    WaitForSingleObject(se.hProcess, 15000); DWORD code = (DWORD)-1; GetExitCodeProcess(se.hProcess, &code); CloseHandle(se.hProcess);
    LogLine("firewall: %s -> exit %lu", cmd.c_str(), code);
    if (FwRuleExists()) Msg(true, "Firewall rule \"" + FwName() + "\" created");
    else Msg(false, Fmt("Firewall rule not found after netsh (exit %lu)", code));
}

// ---------------------------------------------------------------- network self-check
// No peer ping: the mod's only request/answer packet is the lobby HELLO, which a host in Mode=host answers by LOCKING
// its guest slot (net.cpp "the guest slot is taken by the first HELLO") - a probe would take the real guest's place.
volatile LONG g_upnpState = 0;   // 0 idle, 1 running, 2 done
std::string g_upnpMsg;
DWORD WINAPI UpnpThread(void*) {
    uint32_t lan = 0, ext = 0; char name[128] = { 0 }, err[256] = { 0 }; Upnp::Gateway gw{};
    std::string m;
    if (!Upnp::PrimaryIPv4(&lan, name, sizeof(name))) m = "UPnP: no adapter with an internet route";
    else if (!Upnp::Discover(lan, &gw, err, sizeof(err))) m = "UPnP: no gateway answered from " + IpStr(lan) + " (" + err + ") - a host needs a manual port forward or the relay";
    else if (!Upnp::GetExternalIp(gw, &ext, err, sizeof(err))) m = std::string("UPnP: gateway found, external IP query failed (") + err + ")";
    else m = "UPnP: gateway found on " + IpStr(lan) + ", external IP " + IpStr(ext) + " (read only, no mapping made)";
    g_upnpMsg = m; InterlockedExchange(&g_upnpState, 2); return 0;
}
// STUN: one Binding query from an ephemeral UDP socket (not the game port, which the game may hold). Stun::Query
// does up to 3 tries x timeoutMs, so 1000 ms each = 3 s for the exchange; the DNS lookup before it is blocking.
volatile LONG g_stunState = 0;   // 0 idle, 1 running, 2 done
std::string g_stunMsg; bool g_stunOk = false;
const char kStunServer[] = "stun.l.google.com:19302";   // same default as launcher/party_net.h Config::stunServer
DWORD WINAPI StunThread(void*) {
    std::string m; bool ok = false; sockaddr_in srv{};
    if (!Stun::Resolve(kStunServer, &srv)) m = std::string("STUN: could not resolve ") + kStunServer;
    else {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = 0;
        if (s == INVALID_SOCKET || bind(s, (sockaddr*)&a, sizeof(a)) != 0) m = Fmt("STUN: socket/bind failed (%d)", WSAGetLastError());
        else {
            uint32_t ip = 0; uint16_t port = 0;
            if (Stun::Query(s, srv, 1000, &ip, &port)) { ok = true; m = Fmt("STUN: public endpoint %s:%u (ephemeral socket, via %s)", IpStr(ip).c_str(), (unsigned)port, kStunServer); }
            else m = std::string("STUN: no answer from ") + kStunServer + " within 3 s - outbound UDP may be blocked";
        }
        if (s != INVALID_SOCKET) closesocket(s);
    }
    g_stunMsg = m; g_stunOk = ok; InterlockedExchange(&g_stunState, 2); return 0;
}

void SelfCheck() {
    g_check.clear(); RefreshNet();
    static bool wsa = false; if (!wsa) { WSADATA d; wsa = WSAStartup(MAKEWORD(2, 2), &d) == 0; }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) g_check.push_back({ 2, Fmt("Bind: socket() failed (%d)", WSAGetLastError()) });
    else {
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons((u_short)N.localPort);
        if (bind(s, (sockaddr*)&a, sizeof(a)) == 0) g_check.push_back({ 0, Fmt("Bind: UDP %d is free on this PC", N.localPort) });
        else g_check.push_back({ 2, Fmt("Bind: UDP %d is taken (error %d) - close the game or the other program using it", N.localPort, WSAGetLastError()) });
        closesocket(s);
    }
    std::string l; for (uint32_t ip : LocalIPv4s()) l += (l.empty() ? "" : ", ") + IpStr(ip);
    g_check.push_back({ l.empty() ? 2 : 0, "This PC's IPv4: " + (l.empty() ? std::string("none") : l) });
    if (ManualUdp() && !IsLoopback(N.peerIp)) {
        bool bad = false; for (auto& p : g_pre) bad |= p.text.rfind("NET FAIL", 0) == 0;
        g_check.push_back({ bad ? 2 : 0, Fmt("PeerIP %s: %s", N.peerIp, bad ? "NOT on this PC's /24" : "on this PC's /24") });
    } else g_check.push_back({ 1, Fmt("PeerIP not used (Mode=%s, Transport=%s)", N.mode, N.transport) });
    if (wsa && InterlockedCompareExchange(&g_stunState, 1, 0) != 1) {
        InterlockedExchange(&g_stunState, 1);
        HANDLE t = CreateThread(nullptr, 0, StunThread, nullptr, 0, nullptr); if (t) CloseHandle(t); else InterlockedExchange(&g_stunState, 0);
    }
    if (InterlockedCompareExchange(&g_upnpState, 1, 0) != 1) {
        InterlockedExchange(&g_upnpState, 1);
        HANDLE t = CreateThread(nullptr, 0, UpnpThread, nullptr, 0, nullptr); if (t) CloseHandle(t); else InterlockedExchange(&g_upnpState, 0);
    }
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

ImVec4 SevColor(int s);
void NetUi() {
    if (!N.present) { ImGui::TextColored(kDim, "Co-op settings appear after the first install (AC4Coop.ini in the game folder)."); return; }
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.23f, 0.20f, 0.15f, 1)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.31f, 0.27f, 0.20f, 1));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.16f, 0.14f, 0.10f, 1));
    ImGui::TextColored(kDim, "This PC's role ([Coop] EntityHost) - the other PC must pick the other one");
    if (ImGui::RadioButton("Host (EntityHost=1)", N.entityHost != 0) && !N.entityHost) { if (IniSet(L"Coop", L"EntityHost", L"1")) Msg(true, "Role: host ([Coop] EntityHost=1)"); RefreshNet(); }
    ImGui::SameLine();
    if (ImGui::RadioButton("Guest (EntityHost=0)", N.entityHost == 0) && N.entityHost) { if (IniSet(L"Coop", L"EntityHost", L"0")) Msg(true, "Role: guest ([Coop] EntityHost=0)"); RefreshNet(); }

    int mi = ModeIndex();
    ImGui::SetNextItemWidth(140);
    if (ImGui::Combo("[Net] Mode", &mi, kModes, 4) && mi >= 0) { if (IniSet(L"Net", L"Mode", Widen(kModes[mi]))) Msg(true, std::string("[Net] Mode=") + kModes[mi]); RefreshNet(); }
    ImGui::SameLine(); ImGui::TextColored(kDim, "Transport=%s", N.transport);
    static char relay[128], room[32]; static int gen = -1;
    if (gen != g_netGen) { gen = g_netGen; lstrcpynA(relay, N.relay, sizeof(relay)); lstrcpynA(room, N.room, sizeof(room)); }
    ImGui::SetNextItemWidth(260); ImGui::InputText("[Net] RelayServer (host[:port])", relay, sizeof(relay));
    ImGui::SetNextItemWidth(260); ImGui::InputText("[Net] RelayRoom (hex, empty = new)", room, sizeof(room));
    if (ImGui::Button("Save relay")) {
        // net.cpp: RelayRoom = _strtoui64(.., 16) & 0xFFFFFFFFFF (40 bits) - accept 0-10 hex digits only.
        bool hex = strlen(room) <= 10; for (const char* c = room; *c; ++c) hex &= isxdigit((unsigned char)*c) != 0;
        bool sp = false; for (const char* c = relay; *c; ++c) sp |= *c == ' ';
        if (!hex) Msg(false, "RelayRoom must be 0-10 hex digits");
        else if (sp) Msg(false, "RelayServer must be host or host:port, no spaces");
        else if (IniSet(L"Net", L"RelayServer", Widen(relay)) && IniSet(L"Net", L"RelayRoom", Widen(room))) Msg(true, "Relay saved");
        RefreshNet();
    }
    if (N.relay[0] && (ModeIndex() <= 0 || !_stricmp(N.transport, "eos")))
        ImGui::TextColored(kWarn, "RelayServer is used only with Mode=host/join/lobby and Transport=udp (net.cpp)");

    ImGui::TextColored(kDim, "Firewall (runs elevated only after you confirm):");
    ImGui::TextWrapped("netsh %s", FwAddArgs().c_str());
    if (ImGui::Button(Fmt("Allow UDP %d in the firewall", N.localPort).c_str())) AddFirewallRule();
    ImGui::SameLine();
    if (ImGui::Button("Network self-check")) SelfCheck();
    for (auto& c : g_check) ImGui::TextColored(SevColor(c.sev), "%s", c.text.c_str());
    const LONG us = InterlockedCompareExchange(&g_upnpState, 0, 0);
    if (us == 1) ImGui::TextColored(kDim, "UPnP: probing the router...");
    else if (us == 2) ImGui::TextColored(g_upnpMsg.find("external IP ") != std::string::npos ? kOk : kWarn, "%s", g_upnpMsg.c_str());
    const LONG ss = InterlockedCompareExchange(&g_stunState, 0, 0);
    if (ss == 1) ImGui::TextColored(kDim, "STUN: asking %s...", kStunServer);
    else if (ss == 2) ImGui::TextColored(g_stunOk ? kOk : kWarn, "%s", g_stunMsg.c_str());
    for (auto& p : g_pre) ImGui::TextColored(SevColor(p.sev), "%s", p.text.c_str());
    ImGui::PopStyleColor(3);
}
ImVec4 SevColor(int s) { return s == 2 ? kBad : s == 1 ? kWarn : kOk; }

void Ui() {
    ImGui::PushFont(g_big); ImGui::TextColored(kGold, "AC4 CO-OP"); ImGui::PopFont();
    ImGui::TextColored(kDim, "Starts Assassin's Creed IV with the co-op mod. Host or join in the game: press F10.");
    ImGui::Separator();

    ImGui::TextColored(kDim, "Game folder");
    ImGui::TextWrapped("%s", g_gameDir[0] ? g_gameDir : "(not found)");
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.23f, 0.20f, 0.15f, 1)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.31f, 0.27f, 0.20f, 1));
    if (ImGui::Button("Find")) { if (!FindGame()) Msg(false, "Not found in Steam or Ubisoft Connect libraries - use Browse"); SaveSettings(); CheckGame(); RefreshNet(); }
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        char file[MAX_PATH] = "AC4BFSP.exe"; OPENFILENAMEA o{ sizeof(o) }; o.lpstrFilter = "AC4BFSP.exe\0AC4BFSP.exe\0"; o.lpstrFile = file; o.nMaxFile = MAX_PATH; o.Flags = OFN_FILEMUSTEXIST;
        if (GetOpenFileNameA(&o)) { *strrchr(file, '\\') = 0; lstrcpynA(g_gameDir, file, MAX_PATH); SaveSettings(); CheckGame(); RefreshNet(); }
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
    if (ImGui::Button("PLAY CO-OP", ImVec2(260, 52))) { Play(); RefreshNet(); }
    ImGui::PopFont(); ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (!g_msg.empty()) ImGui::TextColored(g_msgOk ? kOk : kBad, "%s", g_msg.c_str());
    ImGui::Spacing(); ImGui::Separator();
    NetUi();
    ImGui::Separator();
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
        else if (a == "--ini") A.ini = next(); else if (a == "--set-role") A.setRole = next();
        else if (a == "--install-dry") A.installDry = next();
        else if (a == "--no-launch") A.noLaunch = true; else if (a == "--auto-play") A.autoPlay = true;
    }
    LocalFree(w);
}
void Report() {
    if (A.report.empty()) return;
    FILE* f = fopen(A.report.c_str(), "w"); if (!f) return;
    fprintf(f, "gameDir=%s\nfound=%d\nknown=%d\npayloadOk=%d\nupToDate=%d\nloader=%s\nloaderNote=%s\nmsg=%s\nmsgOk=%d\nlaunched=%d\n",
            g_gameDir, G.found, G.known, G.payloadOk, G.upToDate, G.loaderName.c_str(), G.loaderNote.c_str(), g_msg.c_str(), g_msgOk, g_launched);
    fprintf(f, "ini=%s\nentityHost=%d\nmode=%s\n", IniPathA().c_str(), N.entityHost, N.mode);
    for (auto& p : g_pre) fprintf(f, "preflight=%d %s\n", p.sev, p.text.c_str());
    fclose(f);
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    ParseArgs(); LoadSettings();
    if (!A.installDry.empty()) {   // test flag: only Install()'s ini-key step on FILE (no copies, no game folder, no window)
        const bool ok = FileExists(A.installDry) && ApplyInstallKeys(A.installDry);
        Msg(ok, ok ? "install-dry: keys applied to " + A.installDry : "install-dry: could not write " + A.installDry);
        LoadNet();   // report mode= from the file (pass --ini FILE too)
        Report();
        return ok ? 0 : 1;
    }
    if (!A.gameDir.empty()) lstrcpynA(g_gameDir, A.gameDir.c_str(), MAX_PATH);
    if (!g_gameDir[0]) FindGame();
    CheckGame();
    RefreshNet();
    if (!A.setRole.empty()) {   // test flag: exactly one [Coop] EntityHost write, same path as the Host/Guest buttons
        const bool host = A.setRole == "host";
        if (host || A.setRole == "guest") { if (IniSet(L"Coop", L"EntityHost", host ? L"1" : L"0")) Msg(true, host ? "Role: host" : "Role: guest"); }
        else Msg(false, "--set-role takes host or guest");
        RefreshNet();
    }
    if (A.autoPlay) Play();

    WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst, LoadIcon(nullptr, IDI_APPLICATION), LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"AC4CoopLauncher", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"AC4 Co-op", WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME), 100, 60, 720, 900, nullptr, nullptr, inst, nullptr);
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
