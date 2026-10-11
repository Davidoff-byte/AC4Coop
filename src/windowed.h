#pragma once
// Windowed-mode enabler, ported from AC4Hook.
// Two parts:
//   1) force scimitar::CommandLineOptions.m_FullScreen = false
//   2) detour scimitar::CreateWindow's style register (esi) so the window is created
//      bordered (WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX) or, optionally, borderless (WS_POPUP)
// Must run at DLL attach, before the engine reaches CreateWindow.

namespace Windowed {
    // fullscreen=false -> windowed.  borderless applies only when windowed.
    void Apply(bool fullscreen, bool borderless);
}
