# Overlay + launcher, first live runs (2026-11-15)

Machine: owner's PC, genuine Steam install `D:\SteamLibrary\...\Assassin's Creed IV Black Flag`, AC4BFSP.exe sha256
`bc11ad79…` (KNOWN). Steam and Ubisoft Connect running. Solo: no second PC, nobody at the keyboard.

## 1. Direct start = store hand-off (17:21, ASI 9dab1b5e, not this work's build)
`AC4BFSP.exe` started directly (pid 30256) exited with code 0 within ~1 s. At ~3 s `UbisoftGameLauncher.exe` (pid 5536,
`-upc_uplay_id 437`) started `AC4BFSP.exe -uplay_steam_mode` (pid 11020), which became the game window.
Consequence: a "start + inject" launcher never reaches the real game process. Hence the arm marker.

## 2. Launcher run 1 (17:34, package 12d5abc, ASI ccbb58a3) - log lost
Chain: pid 27468 (launcher-started) -> 33308 -> 8984 (real game, parent UbisoftGameLauncher 22888).
All three loaded the mod armed. `overlay_log.txt` held only 27468/33308 lines. Restart Manager listed
**UbisoftGameLauncher 22888** as the holder of the file: it had inherited a deny-write handle from a hand-off process,
so every write from 8984 failed. 8984 had `D3DCOMPILER_47.dll` loaded (only the RmlUi DX11 renderer loads it).
Fixed in 9ead987 (log per line, non-inheritable, `_SH_DENYNO`).

## 3. Launcher run 2 (17:38, package 9ead987, ASI 8cda92f8) - PASS for hook, UI init, arm
Real game pid 20088 (overlay_log.txt, its own lines):
- IAT 0x01E3A618 held dxgi!CreateDXGIFactory1 and was patched in DllMain; factory CreateSwapChain wrapped.
- Game swap chain captured: 1920x1071, fmt 28 (R8G8B8A8_UNORM), buffers 1, windowed 1, swapeffect 0, flags 0x2.
  This matches the desc that IDA shows in sub_949FA0.
- RmlUi 6.4-dev ready, both Lato faces loaded, window subclassed. The game's ResizeBuffers 1920x1080 returned hr=0
  through our wrapper.
- 60 presents/s with the hook in place (t=10 s: 364, t=15 s: 663).
- DirectInput: GetDeviceState 1325 calls, GetDeviceData 0. So the game reads devices by state, not buffered data.
- Window messages: 0 mouse / 0 key (nobody at the keyboard). Whether the game window receives WM_MOUSEMOVE is UNTESTED.

## 4. Negative control: plain Steam start (17:40, ASI 8cda92f8, marker deleted)
`steam://rungameid/242050` started hand-off pid 31076, then game pid 16208. The beacon got "not started by the co-op
launcher - inert (plain game)" twice, once per process. There were no new overlay lines and no phase1 log for either
pid. Only `AC4Coop_build_<pid>.log` was written, because the build gate runs before the arm check.

Note: during setup, a Steam start made while the previous armed run's marker still existed (that run lived < 60 s, so
it never disarmed) would have been armed. That attempt was killed and not counted.

## Not tested yet
Opening the panel (F10), drawing in game, mouse/keyboard on the panel, DirectInput zeroing while open, Host/Join and
the bridge from the panel, the role lock, a two-PC session.

## 5. Panel driven live (owner-approved input: F10, mouse, keys; screenshots)
Build 0188bad (ASI 80c11198), title menu, pid 3008, 17:52:
- F10 opened the panel, drawn over the running menu.
- DirectInput devices are **W**. The mouse reads a 20 B state (DIMOUSESTATE2), the keyboard 256 B (logged per device).
- The game window gets **no mouse window messages** (wm mouse=0 throughout), so the panel cursor comes from the DI
  mouse deltas. With the A-struct GetDeviceInfo bug (fixed in 0188bad) the cursor never moved.
- Input blocked while open: the game's own menu cursor froze. Closed: it follows the same injected relative moves.
- Host clicked: `action host`, eos_bridge started (pid 8624), the panel showed code **KAHFHN** plus Copy / Stop, and the
  Join section was hidden. Stop clicked: `action stop`, the bridge exited, state=stopped.

Build a9f7a2d (ASI f66df9a3), pid 4996, 18:10:
- **Title menu: F10 ignored** ("F10 ignored: not in a loaded save"), no panel.
- Save loaded through the single-player tile, straight into Havana docks (no save menu).
- **In the save: F10 opened the panel.** Clicking the code box and typing `ab12c9` showed it in the box. Keyboard
  window messages do reach the window (wm key=12 = 6 keydown + 6 char). F10 closed the panel, back to the game.

Still untested: Join to a real host (needs the second PC), the role lock on connect, the panel closing when a save
unloads, WASD not moving Edward while open (DI zeroing is measured, the movement itself was not tried).
