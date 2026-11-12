#pragma once
// Lobby UI (docs/design/lobby-code.md, slice 6): the code in the window title and on the clipboard, and
// the F7/F8 keys. Plain Win32, no engine calls. Driven from the dllmain poll thread (~20 Hz).
namespace LobbyUi {
    // Call every poll-loop iteration. Does nothing until Net::Init has run in host or join mode.
    void Tick();
}
