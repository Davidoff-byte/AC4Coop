# Same-PC co-op: host + guest as two game copies on one PC (2026-11-15 19:49-19:55)

Build: AC4BFSP.exe sha256 bc11ad79... (pinned), `[Build] asi=f8532f86ea0ce890f0339b38525eeef16bd2d732036d92d5a3894a89986eeaa6
commit=e720202 tree=dirty` (both copies), `proxy/version` version.dll sha256 3f357d02.... Install:
`D:\SteamLibrary\steamapps\common\Assassin's Creed IV Black Flag`. Saves backed up first to
`bf-coop/backups/saves-437-20261115-194916` (with the ini). Verdict: **same-PC two-instance tested** (not two-machine).

## How it was run
- Both copies were started straight from the exe (`Start-Process AC4BFSP.exe`, no Steam, no arguments).
- Shared ini, `[Net] Enabled=1 Mode=host LocalPort=47801 PeerPort=47801`, `[Coop] AutoContinue=1`, `[Pilot] Enabled=1`.
- Copy A, pid 24232: started with `Mode=host`. LAN code `R2M4-8S5T-Q74V` = 192.168.68.100:47801, about 5 s after start.
- Copy B, pid 33312: the ini was switched to `Mode=join JoinCode=R2M4-8S5T-Q74V` just before B started. B read it at its
  `Net::Init` (`[Net] cfg ... mode=join joinCode=R2M4-8S5T-Q74V`), and the ini was then restored (sha256 equal to the original).
- B logged `[Net] instance slot=2 not host`. As designed in `net.cpp` Init, the slot-2 join copy bound an ephemeral port:
  192.168.68.100:54332. A bound 192.168.68.100:47801 (`Get-NetUDPEndpoint`).
- Both copies stopped at the main menu with `AutoContinue` alone (screenshot: single-player tile). `tools/pilot.py --pid <pid> boot
  --pulses 4 --wait 6` brought each one into the world (virtual pad, no OS input): A frame 711, 102 characters, pos (76.0, -74.0, 1.8);
  B frame 708, 103 characters, pos (75.9, -73.9, 1.8).

## Results
- **Handshake** (net_log): `[Lobby] guest joined 192.168.68.100:54332`, the exact socket B owns. Window titles: A "guest joined (direct)",
  B "connected (direct)".
- **Traffic in the world** (`[Net] stats`, one 5 s window, both in the world):
  - A: tx 405 pkt / 246,190 B / 1334 recs, rx 306 pkt / 173,516 B (48.0 kB/s up, 33.8 kB/s down).
  - B: tx 306 pkt / 173,516 B, rx 405 pkt / 246,450 B (33.7 kB/s up, 47.9 kB/s down).
  - Packet counts match across the two copies in both directions (405/405, 306/306), and so do B tx bytes and A rx bytes, to the byte.
    A tx bytes and B rx bytes differ by 260 B, probably because the two windows ended at different moments (unverified).
- **Puppets:** A `[Sight] host=1 puppet=3A20E870`, B `[Sight] host=0 puppet=3AA052D0`. `[Own] puppet frames=300/301 moved=100%`
  on both.
- **Entity mirror on the guest:** `mirror ok=950 nobod=300 steal=50 rejects=50`, plus `[MirrorAct]` lines.

## Notes and gaps
- With only `AutoContinue`, the link comes up on the main menu's background world, and `[Net] stats` is not logged until the copy
  is in the world. Every recent single-copy log had 0 stats lines for the same reason (they never left the menu).
- The ini was rewritten at 19:49:53, while my restore call raised "locked a portion of the file". The plugin has no
  `WritePrivateProfileString` call, and the final content equals the original. The write most likely landed and the error came
  afterwards (unverified).
- **Not checked:** puppet appearance on screen (only A's frame was captured, at the menu), combat or kills between the copies, any
  run longer than about a minute in the world, and both copies writing the same save slot.
