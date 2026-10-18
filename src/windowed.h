#pragma once
// Windowed-mode enabler.
//   1) keep scimitar::CommandLineOptions.m_FullScreen (and the byte the engine preloads) false
//   2) detour the style register in scimitar::CreateWindow so the window is created bordered
//      (WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX) or, if asked, borderless (WS_POPUP)
// Has to run at DLL attach, before the engine reaches CreateWindow.

namespace Windowed {
    // fullscreen=false -> windowed.  borderless applies only when windowed.
    void Apply(bool fullscreen, bool borderless);
}
