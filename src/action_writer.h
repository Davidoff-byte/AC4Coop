#pragma once
// Action-writer probe: which engine path switches a mirrored guard's action away from the host's.
// See action_writer.cpp for the hook site and the evidence it rests on. [Coop] ActionWriterProbe (default 0,
// read at load). Observation only: it never blocks or changes a switch.
#include <cstdint>

namespace ActionWriter
{
    void Init();
    bool Installed();
    // Mark `ac` (an ActionComponent*) as a body we drive, with the host's current alias. Called from the body's
    // own update every frame; an entry not refreshed for 1 s is ignored.
    void NoteMirrored(void* ac, uint32_t hostAlias);
    // Set around our own action writes on this thread, so they are counted as ours.
    void SetOwn(bool on);
    // One summary line for the last window (counters reset). Returns 0 when not installed.
    int  FormatReport(char* out, int cap);
}
