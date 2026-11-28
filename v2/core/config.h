#pragma once
// v2 typed config (ARCHITECTURE.md s4, R5). Read once at init from AC4Coop.ini next to the ASI (Paths::Ini), printed in
// full, never re-read. Defaults are the verified values; each key carries its evidence in config.cpp.
#include <cstdint>

namespace v2 {

struct Config {
    // [Display] (boot, before the engine creates its window)
    int  fullscreen;          // Fullscreen
    int  borderless;          // Borderless
    // [Launcher]
    int  requireArm;          // RequireArm: inert unless started by the co-op launcher (AC4Coop.armed < ArmWindowSec old)
    int  armWindowSec;        // ArmWindowSec
    // [Boot]
    int  multiInstance;       // MultiInstance: unique mutex per process (two copies on one PC)
    int  skipIntro;           // SkipIntroVideos
    int  autoContinue;        // AutoContinue: title "press start" + continue the save
    int  autoContinueSlot;    // AutoContinueSlot (-1 = most recent)
    int  noPause;             // NoPause: keep simulating when the window loses focus
    // [Net]
    char netMode[16];         // Mode: manual | host | join
    char peerIp[64];          // PeerIP
    int  peerPort;            // PeerPort
    int  localPort;           // LocalPort
    char bindIp[64];          // BindIP
    char joinCode[32];        // JoinCode
    // [Coop]
    int  players;             // Players
    int  avatar, guards, combat, aiShips, peerShip, naval, boarding, worldTime;
    int  guestShipTraffic;    // GuestShipTraffic: 0 = the guest declines its own naval spawns in the shared area
    int  puppetDelayMs, guardDelayMs, aiShipDelayMs, peerShipDelayMs;
    int  syncRadiusM, shipSyncRadiusM;
    char alertIds[32];        // AlertIds
    // [Log]
    int  logLevel;            // Level: 0 debug, 1 info, 2 warn, 3 error
    // [Pilot] (DevTools build only)
    int  pilot;               // Enabled
    int  pilotKeepFocus;      // KeepFocus
    int  pilotPadIndex;       // PadIndex
};

const Config& Cfg();
void          LoadConfig(const char* iniPath);   // DllMain attach, before any hook
void          LogConfig();                       // after the logger starts: every key, value, source

}  // namespace v2
