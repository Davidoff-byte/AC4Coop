#pragma once
// In-game overlay: hook layer (docs/design/eos-transport.md, "In-game overlay"). Knows D3D11/DXGI, Win32 and
// DirectInput, never RmlUi (that is overlay_ui.*).
//   [Overlay] Probe=0   1 = hook the game's swap chain and log present/input counts only (no drawing)
//   [Overlay] Enable=0  1 = the co-op panel (implies Probe); toggle key [Overlay] Key (default 0x2D = Insert; F10 is
//                       the always-on ship-sink harness key in dllmain.cpp). UI: Dear ImGui by default (overlay_ui.h)
// Swap chain capture, verified in IDA on AC4BFSP.exe:
//   the game's only CreateDXGIFactory1 caller is sub_968150 (call 0x968181, thunk 0x976220, IAT 0x01E3A618);
//   sub_949FA0 creates the swap chain with IDXGIFactory::CreateSwapChain (vtable +0x28, call 0x0094A1DD) on that
//   factory's object, desc: R8G8B8A8_UNORM (28), BufferCount 1, DISCARD, Windowed 1, ALLOW_MODE_SWITCH.
// So the IAT slot is swapped at DllMain (before WinMain), the returned factory's CreateSwapChain slot is wrapped,
// and the game's own swap chain gets Present (+0x20) and ResizeBuffers (+0x34) wrapped. No dummy device.
namespace Overlay {
void InstallEarly();   // DllMain, after the build gate: the IAT swap (no thread, no file I/O beyond the log)
void Start();          // poll thread: DirectInput hooks + the status/log thread
}
