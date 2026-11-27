# In-game overlay (co-op panel)

Keys: `[Overlay] Enable=0` (panel), `Probe=0` (hook + counts only), `Key=45` (0x2D Insert), `RoleFromPanel=0`.
UI: Dear ImGui v1.91.5 (`third_party\imgui`, `src\overlay_ui_imgui.cpp`) by default; RmlUi (`src\overlay_ui.cpp`)
only with `/p:Ac4UiRmlUi=1` and `local\rmlui-x86` built; otherwise a stub. Both sit behind `src\overlay_ui.h`.

## Hook chain (as cited in `src\overlay.h`, IDA on AC4BFSP.exe; not re-verified in this slice)
1. DllMain (`Overlay::InstallEarly`): IAT slot `0x01E3A618` (CreateDXGIFactory1; only caller `sub_968150`, call
   `0x968181`) is swapped only if it still holds `dxgi!CreateDXGIFactory1`.
2. The returned factory's `CreateSwapChain` (vtable +0x28; game call `0x0094A1DD` in `sub_949FA0`) is wrapped.
3. The game's swap chain gets `Present` (slot 8, +0x20) and `ResizeBuffers` (+0x34) wrapped. No dummy device.
4. First Present with Enable=1: `GetDevice`, `OverlayUi::Init`, WndProc subclass. RTV from buffer 0, released
   before ResizeBuffers. D3D state backed up / restored around the UI draw. SEH: an exception disables the overlay.

## Thread rules
- Render thread (Present wrap): ImGui frame + draw only. No engine calls, no file/ini I/O (`io.IniFilename=nullptr`).
- Window thread (WndProc subclass): queues input messages only while the panel is open; everything passes through
  when closed. DirectInput hooks (`c_diZeroed`) feed the cursor while open; that path was not re-read here.
- Overlay thread (250 ms): runs queued button actions (host/join/copy/stop, same handlers as RmlUi) and builds the
  Status, incl. read-only rows: `NET_PROTO_VER`, `[Build]` commit/asi, `Phase1_PuppetEntity()!=nullptr`,
  `Phase1_ShipBindCount()` (s_scbN), `Phase1_GuardBindCount()` (s_bindBound, current 5 s window).
- Toggle key: F10 was the default, but `dllmain.cpp` F10 always runs `RequestShipTest(2)` (sink nearby ships),
  ungated. Default moved to Insert; no `GetAsyncKeyState`/`GetKeyState` in `src\` reads VK_INSERT. Whether the game
  itself binds Insert is UNVERIFIED.

## PASS/FAIL bar for the first live run (not yet run)
Enable=1, load a save, press Insert twice. PASS iff `overlay_log.txt` has, in order:
1. `[Overlay] ---- pid=… enable=1 key=0x2d ui=imgui` and `IAT CreateDXGIFactory1 patched=1`
2. `swap chain vtable … patched present=1 resize=1`, `game swap chain …`
3. `[Overlay] ImGui 1.91.5 ready WxH`, `window … subclassed=1`
4. `panel open`, then `panel closed`; the 5 s `t=…s present=N` lines keep growing; no `exception` / `FAILED` line.
5. Screenshot shows the panel with role/state/code/link/peer and the proto/build/puppet/bind rows.
Enable=0 and Probe=0: PASS iff `overlay_log.txt` gets no new lines for that pid and no `[Overlay]` line appears in
any log (InstallEarly returns before the IAT swap, so the frame path is the game's own).
FAIL on any other outcome, including a crash, a black or flickering frame, or input reaching the game while open.
