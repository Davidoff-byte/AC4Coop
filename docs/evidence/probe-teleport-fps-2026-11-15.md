# [Probe] tap port, [Pilot] teleport + frame cap: live check, 2026-11-15

Verdict: **solo-tested**. Game AC4BFSP.exe md5 2058342866688f780c8b34526a65bc35 (Steam).

## Failed run first (kept as a result)
Build 9ec7dfc (asi c05a1264...) died 2 s after start: Windows Application log event 1000, `AC4BFSP.exe`, faulting module
ntdll.dll, exception **0xC00000FD** (stack overflow), pid 0x2834 = 10292, 01:04:24. The log stopped after
`[Pilot] focus keeper on window ...`. That build added at start (a) a vectored exception handler that read a
`__declspec(thread)` variable on every single-step exception and (b) a speculative IDXGISwapChain::Present patch.
Fix 3b1670a removed both from the start path (TlsAlloc storage, handler installed on the first WWATCH, Present patched
only when a cap is requested). **Which of the two caused the overflow is UNVERIFIED** (no A/B run).

## Run 11912, build 3b1670a, asi 02f240a2388ea58d5319bbec795d7e8cb3eb4bea078ac3446932ab6b8382471a
ini: `[Pilot] Enabled=1 KeepFocus=1 PadIndex=0`, `[Probe] Enabled=1`, `[Coop] AutoContinue=1`.

| check | command | observed |
|---|---|---|
| start | game launch | stable; `[Probe] on: pipe \\.\pipe\ac4probe-11912, ring 4096 events x 1136 bytes` |
| boot + cap | `pilot.py boot --fps 60` | `frame cap 60.0: status 0`; 2 A pulses; in world, 102 characters; `fps`: cap=60 measured=59 presentPatched=1 |
| hook with exit capture | `probe.py HOOK 0 70e810 8b81f80700008b48 getplayer exit` | ~1 s: entries 16743 = exits 16743; entry ecx 0x2950ED50 = Pilot `world`, exit eax 0x4072A510 = Pilot `player`; exit esp = entry esp + 4 (thiscall, no stack args); game alive after UNHOOK |
| teleport | `pilot.py teleport 70.9 -70.3 1.8`, then `76 -74 1.8 --yaw 90` | both held 2 s later at the target (z settled 1.75 / 1.78); after yaw 90: row0 (1,0,0), row1 (0,1,0) |
| camera-relative stick (known limit) | `stick 0 1` after the yaw-90 teleport | Edward turned to (-0.91,0.41): the stick follows the camera, not his new facing |

Not tested yet: WWATCH write tracing, SNAP expressions, POKE, UNLOAD; a negative control with both flags 0.
