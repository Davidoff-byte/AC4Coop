# S0: two-copy test rig and the pilot frame-cap exits (2026-11-15 20:17-21:27)

Slice S0 of `docs/design/storm-replication.md`. Build: AC4BFSP.exe pinned (bc11ad79...); ASI
66d3520444d5ecec156daa0ec3a502bf6a1dccb7d1163b513bbc4f3698807eb2 (`a29b8ab`, tree clean); `proxy/version`
version.dll 3f357d02.... D: Steam install. Every run: `[Coop] NativeReplica=0 ActMatchProbe=1`, `tools/two_copy_moves.py` on
the host.

## Rig
- `tools/two_copy.ps1`: host + guest started directly from the exe, lobby-code join, `pilot.py boot`, window placement
  (`tools/arrange_windows.ps1 -Resize`: host top-right, guest bottom-left, 960x540 client areas), optional host moves,
  `[ActMatch]` and `[Net] stats` collection.
  - Hang watchdog: not responding and under 0.5 s of CPU in 3 s means hung; the copy is dumped with `tools/minidump.py`
    to `D:\AC4-RE\dumps`.
  - The ini is restored to its starting bytes in every case (`finally`).
- `tools/minidump.py`: tested on a throwaway process (51 KB, valid `MDMP` header).

## Oracle: 3 consecutive clean runs, `--fps 0`
| run | start | host / guest | boot | moves | hang or exit | ini restored |
|---|---|---|---|---|---|---|
| 1 | about 21:05 (moves 21:08:02) | 33656 / 8604 | host on try 2, guest on try 1 | done | none | yes |
| 2 | 21:08:55 | 29940 / 25308 | host on try 2, guest on try 1 | done | none | yes |
| 3 | 21:12:36 | 26796 / 29712 | host on try 2, guest on try 1 | done | none | yes |

**PASS.** The host's first boot attempt reports "not in world yet" in every run, with or without the cap, and the retry
succeeds. The host simply takes longer to load. Run 1 guest `[ActMatch]`: 84.6-100% in every context (ctx 4/6/7/8/9/30),
consistent with `act-match-ab-2026-11-15.md`.

## Frame cap (`pilot.py boot --fps 60` patches DXGI Present)
| when | arm | result |
|---|---|---|
| 20:17 | replica on, cap | host stalled on boot try 1 (pos 0,0,0 while moves ran), run discarded |
| 20:21 | replica on, cap | clean |
| 20:22 | replica off, cap | guest stopped responding right after `[Pilot] frame cap: ... patched` (0.12 s CPU in 3 s); closed before a dump |
| 20:31 | replica off, cap | host log ends at the patch line; frame counter stuck at 2; process exited, no Application-log entry |
| 21:16 | replica off, cap | host 10104 log ends at the patch line (line 245, menu); exited during boot try 2 |
| 21:20 | replica off, cap | clean |
| 21:23 | replica off, cap | guest 22296 patched in the world (line 1117), exited during the moves; last line a puppet action write |
| 20:37, 20:40 (interactive), runs 1-3 above | `--fps 0` | 5 of 5 clean |

- With the cap: 7 runs, 4 lost copies, 3 of them silent exits. Without the cap: 5 runs, 0 lost copies.
- Exit-at-patch happened twice (both at the menu). The 21:23 exit came minutes later, so its cause is open (cap or the
  action write).
- Static reading of `src/pilot.cpp` `TryPatchPresent` (from line 233): on the plugin's poll thread
  (`dllmain.cpp:371`, `Pilot::Poll`), it creates a hidden STATIC window and a second D3D11 device + swap chain, patches
  swap-chain vtable slot 8, then releases them while the game renders. A deadlock or teardown race inside DXGI is a
  candidate explanation. UNVERIFIED: no dump; silent exits leave nothing for the watchdog.
- **Change:** `tools/pilot.py boot` now defaults to `--fps 0`; `tools/two_copy.ps1` passes `-Fps 0` by default. A fix
  for the patch itself (hook the game's own swap chain, no throwaway device) is open.

## Not checked
The cause of the silent exits (a debugger with first-chance exception capture would catch them); the guest window in run 3
came out 964x567 outer (one-off, not re-measured).
