#pragma once
// Menu probe ([Coop] MenuProbe=1, default 0): observation-only prologue hooks on the title-menu save path
// (see menu_probe.cpp). Writes AC4Coop_menu_probe_<pid>.log.
namespace MenuProbe {
    void Init();   // DllMain attach, after the build gate (like CombatObserve::Init)
}
