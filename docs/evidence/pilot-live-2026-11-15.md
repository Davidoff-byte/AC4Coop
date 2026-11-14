# [Pilot] live check, 2026-11-15

Build: commit 04b314b (tree clean), asi sha256 b01f046298f656dd963b752d4d2490d0a510e7adf3a87c47e62a0284a24a2704
(`[Build]` line of phase1_log_24540.txt). Game AC4BFSP.exe md5 2058342866688f780c8b34526a65bc35, Steam install.
ini: `[Pilot] Enabled=1 KeepFocus=1 PadIndex=0`, `[Coop] AutoContinue=1`. Run pid 24540. Verdict: **solo-tested**.

| step | command | observed |
|---|---|---|
| load | game start | `[Pilot] imports patched: XInputGetState=1 SetState=1 GetCapabilities=1 GetForegroundWindow=1 GetActiveWindow=1`; `[Pilot] focus keeper on window 005301E2` |
| pad polled | `pilot.py status` (main menu) | `polls=1359 served=0`, menu scene: 1 character |
| boot | `pilot.py boot --pulses 4 --wait 6` | 2 A pulses (single-player tile, save slot), then `in world: frame 708, 102 characters, pos (76.0, -74.0, 1.8)`; no keyboard or OS input used |
| scan | `pilot.py scan --r 30` | 31 of 101 characters with kind/role (Peasant, Courtesan ...) and distance |
| stick | `pilot.py stick 0 1 --sec 1.5` | (76.00,-74.00) → (74.68,-75.33): 1.9 m along matrix row 1 (-0.71,-0.71) |
| goto | `pilot.py goto 70 -70 --tol 1` then `goto 78 -76` | arrived d=1.00 m in 6.2 s, d=0.98 m in 6.8 s; learned camera offset 34° / 38° |
| memory | `pilot.py mem player 0xA0` | entity +0x10..+0x3F rotation rows, +0x40 position (77.32,-75.39,1.86) |
| visual | desktop screenshot while the game window was unfocused | Edward standing in Havana, game rendering |
| errors | phase1_log | 0 `EXCEPTION` lines |

Observed, not yet established: matrix row 1 = Edward's facing (one stick test, straight walk); the camera offset is
learned from motion, not read. Not tested: KeepFocus with another window actively focused for a long time; negative
control with `[Pilot] Enabled=0` (expected: no `[Pilot]` lines, no sections).
