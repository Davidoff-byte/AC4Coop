# AC4Coop Launcher: a pre-game party

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
