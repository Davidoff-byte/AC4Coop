#pragma once
// UI probe ([Coop] UiProbe=1, default 0): measures the present path and the input path an in-game
// overlay must hook (see ui_probe.cpp). Diagnostic only; writes ui_probe.txt next to the logs.
namespace UiProbe {
    void Start();   // call once from the poll thread (never from DllMain)
}
