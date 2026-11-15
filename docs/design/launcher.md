# AC4Coop Launcher: the only way co-op runs

Status: **2026-11-15 rewrite (EOS era). Install path checked offline on scratch game folders. Not run with the game
yet.** The party-launcher design below ("pre-game party") is superseded: Host / Join moved into the game (the co-op
panel, F10, docs/design/eos-transport.md), and the launcher now only installs and starts the modded game.

## What it does now
1. **Game**: finds `AC4BFSP.exe` (Steam libraries, Ubisoft Connect installs, or Browse) and checks its sha256 against
   `known_builds.txt`.
2. **Install / update** (on Play when anything differs, by sha256): `AC4Coop.asi`, `known_builds.txt`, the loader,
   `eos\` (64-bit bridge, EOS SDK DLL, x64 VC runtime, `eos.ini` with the client credentials) and `coop_ui\` (panel
   RML/RCSS, fonts). Replaced files are kept as `*.launcher-bak`. A missing `AC4Coop.ini` is created from the
   template, an existing one is kept and only these keys are written (backup `AC4Coop.ini.launcher.bak`):
   `[Launcher] RequireArm=1`, `[Net] Enabled=1 Mode=manual Transport=eos EosAutoStart=0`,
   `[Overlay] Enable=1 RoleFromPanel=1`, plus the role-independent `[Coop]` sync profile.
3. **Loader name**: Ultimate ASI Loader as `dinput8.dll`. When another mod owns a `dinput8.dll` that is not Ultimate
   ASI Loader, ours goes in as `version.dll` (both names are on the loader's Win32 list and AC4BFSP.exe imports both).
   Other `.asi` mods simply load alongside through the same loader.
4. **Play**: refuses while `AC4BFSP.exe` runs, writes `<game>\AC4Coop.armed`, starts `AC4BFSP.exe` from the game folder.

## Separate from the plain game
With `RequireArm=1` the ASI installs nothing unless `AC4Coop.armed` is fresh (`[Launcher] ArmWindowSec`, default 300 s),
so Play in Steam / Ubisoft Connect is the unmodded game. Measured 2026-11-15 17:21 (genuine Steam install, Steam and
Ubisoft Connect running): a direct `AC4BFSP.exe` start (pid 30256) exited with code 0 within ~1 s, and
`UbisoftGameLauncher.exe` started `AC4BFSP.exe -uplay_steam_mode` (pid 11020) ~3 s later, which became the game. So the
marker must outlive the first process: the live game deletes it after 60 s. The same measurement rules out a
Seamless-Co-op-style "start the game and inject" launcher: the process the launcher starts is never the game.

## Offline check (2026-11-15, package AC4Coop-Launcher-0577dca-dirty)
`AC4CoopLauncher.exe --game-dir <scratch> --payload <pkg>\files --no-launch --auto-play --report r.txt`:
- clean folder (only AC4BFSP.exe): all payload files installed, loader `dinput8.dll`, marker written, ini keys as above;
- folder with a foreign `dinput8.dll` (SysWOW64 copy): left untouched, loader installed as `version.dll`.
Packaging: `tools\package_launcher.ps1` (eos.ini comes from the ignored `local\eos.ini`; the package is never committed).

---

# (superseded) AC4Coop Launcher: a pre-game party

Status: **built and checked offline (two launchers + relay on one PC); not run with the game yet.** 2026-11-12.

## What it does
1. **Game**: finds `AC4BFSP.exe` (Steam `libraryfolders.vdf`, Ubisoft Connect `Installs\*\InstallDir`, or Browse),
   checks its sha256 against `known_builds.txt`, installs/repairs the mod files it ships in `files\` next to the exe
   (`AC4Coop.asi`, `dinput8.dll`, `known_builds.txt`; replaced files are kept as `*.launcher-bak`; an existing
   `AC4Coop.ini` is never overwritten, a missing one is created from the template).
2. **Party**: the host clicks *Host a party* and gets a code (a relay room code `Z...` when a relay server is set,
   else a direct code from UPnP + STUN, plus a LAN code). Friends paste it and *Join*. Everyone sees the roster.
3. **Start**: the host clicks *Start game for everyone*. START goes to every guest (repeated over 2 s). Each launcher
   writes its game's `AC4Coop.ini` `[Net]` keys (backup `AC4Coop.ini.launcher.bak`) and starts `AC4BFSP.exe` from the
   game folder (the same exe for Steam and Ubisoft Connect):
   - host: `Mode=host`, `RelayServer`, `RelayRoom=<room id>` (the plugin re-registers that room)
   - guest: `Mode=join`, `JoinCode=<host code>`, `RelayServer` — the game joins by itself in the world (10-min retry)
4. The launcher used the same UDP port as the game (`LocalPort`, 47801) and releases it before the game starts, so
   the NAT / UPnP mappings - and with them the code - carry over.

## Pieces
- `src/party_proto.h`: the party wire protocol ('AC4P' header 12 B; JOIN 260 B padded so no reply is larger;
  ROSTER 228 B; START 100 B; REFUSE 4 B; static_asserted).
- `launcher/party_net.{h,cpp}`: one socket, one receive thread (also completes STUN), host setup thread (relay
  REGISTER, UPnP, STUN, verdict, codes), host upkeep (relay keepalive, 10 s guest timeout), guest thread (relay JOIN
  for Z codes, party JOIN, PING every 2 s, 15 s host timeout), START repeats.
- `launcher/main.cpp`: Dear ImGui v1.91.5 on Win32 + D3D11; settings in `launcher.ini` next to the exe.
- Plugin (`src/net.cpp`): `[Net] RelayRoom` for the host; a guest's relay join keeps retrying "no such room" within
  its 10-min window (the room is briefly gone between the launcher closing it and the host's game re-registering).

## Limits (honest)
- The game is 2-player: the roster takes up to 8, extra members are shown as waiting.
- A direct (non-relay) code carries over only while the host's NAT keeps the mapping: with UPnP verdict OK it is
  fixed; otherwise a relay server is the reliable choice.
- No Python twin of the party protocol (deviation from the plan): the oracle is two real launcher instances.

## Verified (offline, one PC; launchers with `--no-launch`, fake game folders holding a copy of the real exe)
- Direct: host Alice + guest Bob -> roster 2 on both; START -> host ini `Mode=host`, guest ini `Mode=join
  JoinCode=<host code>`; diff of the guest ini vs the template: only `[Net]` keys changed. Screenshots inspected.
- Relay (`ac4coop-relay` on 127.0.0.1): room code `Z...`; guest joined through the server; host ini
  `RelayRoom=<id>`, guest ini `JoinCode=<Z code>`.
- Plugin harness: a guest started 20 s before the host logged "room is not there yet", then joined when the host
  registered the configured `RelayRoom` id (same code), ~300 datagrams each way, 0 rejects.
