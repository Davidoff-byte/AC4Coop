#include "config.h"
#include "log.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace v2 {
namespace {

Config g_cfg;
char   g_ini[MAX_PATH] = {};

enum Type : uint8_t { kInt, kStr };

struct Key {
    const char* section;
    const char* key;
    Type        type;
    int         defInt;
    const char* defStr;
    size_t      offset;
    size_t      cap;          // string capacity
    const char* evidence;     // why this default
    uint8_t     fromIni;      // set by LoadConfig
    uint8_t     bad;          // an int key whose ini text is not a number (the default is used)
};

#define I(sec, k, field, def, ev) { sec, k, kInt, def, nullptr, offsetof(Config, field), 0, ev, 0, 0 }
#define S(sec, k, field, def, ev) { sec, k, kStr, 0, def, offsetof(Config, field), sizeof(Config::field), ev, 0, 0 }

Key g_keys[] = {
    I("Display",  "Fullscreen",       fullscreen,       0,    "windowed for two copies on one PC (v1 s0-two-copy-rig)"),
    I("Display",  "Borderless",       borderless,       0,    "v1 default"),
    I("Launcher", "RequireArm",       requireArm,       0,    "v1: plain-game start stays inert when 1"),
    I("Launcher", "ArmWindowSec",     armWindowSec,     300,  "v1 default"),
    I("Boot",     "MultiInstance",    multiInstance,    1,    "v1 single-instance-guard-2026-11-15: two copies"),
    I("Boot",     "SkipIntroVideos",  skipIntro,        1,    "v1 install value"),
    I("Boot",     "AutoContinue",     autoContinue,     1,    "v1 install value (boot straight into the save)"),
    I("Boot",     "AutoContinueSlot", autoContinueSlot, -1,   "v1 default"),
    I("Boot",     "NoPause",          noPause,          1,    "v1 InstallNoPauseHook (always installed)"),
    S("Net",      "Mode",             netMode,          "manual", "v1 PL two-machine transport"),
    S("Net",      "PeerIP",           peerIp,           "127.0.0.1", "same-PC rig"),
    I("Net",      "PeerPort",         peerPort,         47801, "v1 code default; two PCs need equal ports"),
    I("Net",      "LocalPort",        localPort,        47801, "v1 code default"),
    S("Net",      "BindIP",           bindIp,           "",   "any"),
    S("Net",      "JoinCode",         joinCode,         "",   "lobby code (Mode=join)"),
    I("Coop",     "Players",          players,          2,    "owner 2026-11-28: two now, N-ready"),
    I("Coop",     "Avatar",           avatar,           1,    "v1 PL puppet"),
    I("Coop",     "Guards",           guards,           1,    "v1 PL guard lane"),
    I("Coop",     "Combat",           combat,           1,    "v1 PL damage/kill lanes"),
    I("Coop",     "AiShips",          aiShips,          1,    "v1 PL ShipClassBind + ShipInterp"),
    I("Coop",     "PeerShip",         peerShip,         1,    "v1 PL ghost"),
    I("Coop",     "Naval",            naval,            1,    "v1 ShipStateSync PASS n=1"),
    I("Coop",     "Boarding",         boarding,         1,    "phase 5"),
    I("Coop",     "WorldTime",        worldTime,        1,    "v1 ToD n=1"),
    I("Coop",     "GuestShipTraffic", guestShipTraffic, 0,    "v1 owner-issues 2026-11-28 T1 PASS"),
    I("Coop",     "PuppetDelayMs",    puppetDelayMs,    100,  "v1 InterpDelayMs"),
    I("Coop",     "GuardDelayMs",     guardDelayMs,     150,  "v1 GuardInterpBuffer"),
    I("Coop",     "AiShipDelayMs",    aiShipDelayMs,    400,  "v1 s15: 400 ms 0.25 % vs 300 ms 5.0 %"),
    I("Coop",     "PeerShipDelayMs",  peerShipDelayMs,  250,  "v1 s15: 0.0 %"),
    I("Coop",     "SyncRadiusM",      syncRadiusM,      60,   "v1 install"),
    I("Coop",     "ShipSyncRadiusM",  shipSyncRadiusM,  1000, "v1 ShipRelevanceRadiusM"),
    S("Coop",     "AlertIds",         alertIds,         "8,257", "v1 conflict-gate-2026-11-17 s6 run 8"),
    I("Log",      "Level",            logLevel,         1,    "info"),
    I("Pilot",    "Enabled",          pilot,            0,    "DevTools only"),
    I("Pilot",    "KeepFocus",        pilotKeepFocus,   0,    "DevTools only"),
    I("Pilot",    "PadIndex",         pilotPadIndex,    0,    "DevTools only"),
};
#undef I
#undef S

bool Known(const char* section, const char* key) {
    for (const Key& k : g_keys) if (_stricmp(k.section, section) == 0 && _stricmp(k.key, key) == 0) return true;
    return false;
}

// R5: keys in the ini that v2 does not read are ignored, and reported once: a count per section at info, every name at
// debug. A v1 ini (358 keys) installed under v2 shows up here instead of silently half-applying.
void ReportUnknown() {
    if (!g_ini[0] || GetFileAttributesA(g_ini) == INVALID_FILE_ATTRIBUTES) return;
    static char secs[8192], body[65536];
    if (!GetPrivateProfileSectionNamesA(secs, sizeof(secs), g_ini)) return;
    unsigned total = 0;
    char summary[400] = {}; size_t used = 0;
    for (const char* s = secs; *s; s += strlen(s) + 1) {
        unsigned n = 0;
        if (GetPrivateProfileSectionA(s, body, sizeof(body), g_ini)) {
            for (const char* e = body; *e; e += strlen(e) + 1) {
                if (*e == ';' || *e == '#') continue;
                const char* eq = strchr(e, '=');
                char key[96] = {};
                strncpy_s(key, e, eq ? (size_t)(eq - e) : strlen(e));
                for (size_t i = strlen(key); i > 0 && (key[i - 1] == ' ' || key[i - 1] == '\t'); --i) key[i - 1] = 0;
                if (!key[0] || Known(s, key)) continue;
                ++n;
                LOGD("[Config] ignored [%s] %s\n", s, key);
            }
        }
        if (!n) continue;
        total += n;
        const int w = _snprintf_s(summary + used, sizeof(summary) - used, _TRUNCATE, "%s[%s] %u", used ? ", " : "", s, n);
        if (w > 0) used += (size_t)w;
    }
    if (total) LOGW("[Config] %u ini key(s) are not v2 keys and are ignored: %s\n", total, summary);
    else LOGI("[Config] no unknown ini keys\n");
}

}  // namespace

const Config& Cfg() { return g_cfg; }

void LoadConfig(const char* iniPath) {
    strncpy_s(g_ini, iniPath ? iniPath : "", _TRUNCATE);
    const bool have = g_ini[0] && GetFileAttributesA(g_ini) != INVALID_FILE_ATTRIBUTES;
    for (Key& k : g_keys) {
        char* field = (char*)&g_cfg + k.offset;
        char buf[128] = {};
        const DWORD n = have ? GetPrivateProfileStringA(k.section, k.key, "\x01", buf, sizeof(buf), g_ini) : 0;
        const bool present = have && !(n == 1 && buf[0] == '\x01');
        k.fromIni = present ? 1 : 0;
        if (k.type == kInt) {
            int v = k.defInt;
            if (present) {
                char* end = nullptr; const long p = strtol(buf, &end, 0);
                if (end != buf) v = (int)p; else k.bad = 1;
            }
            memcpy(field, &v, sizeof(v));
        } else {
            strncpy_s(field, k.cap, present ? buf : k.defStr, _TRUNCATE);
        }
    }
}

void LogConfig() {
    LOGI("[Config] %s (%u keys; '*' = from the ini)\n", g_ini[0] ? g_ini : "(no ini)", (unsigned)(sizeof(g_keys) / sizeof(g_keys[0])));
    for (const Key& k : g_keys) {
        const char* field = (const char*)&g_cfg + k.offset;
        if (k.type == kInt) LOGI("[Config] %c [%s] %s=%d  ; %s\n", k.fromIni ? '*' : ' ', k.section, k.key, *(const int*)field, k.evidence);
        else LOGI("[Config] %c [%s] %s=%s  ; %s\n", k.fromIni ? '*' : ' ', k.section, k.key, field, k.evidence);
        if (k.bad) LOGW("[Config] [%s] %s: the ini value is not a number; the default is used\n", k.section, k.key);
    }
    ReportUnknown();
}

}  // namespace v2
