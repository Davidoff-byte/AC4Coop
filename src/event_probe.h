#pragma once
// Event probe: which events the local player's actions post (shooting, attacking), so the host can later raise the
// same event for the co-op puppet. See event_probe.cpp for the hook site. [Coop] EventProbe (default 0, read at
// load). Observation only: it never changes or drops an event.

namespace EventProbe
{
    void Init();
    // One summary line for the last window (counters reset). Returns 0 when not installed.
    int  FormatReport(char* out, int cap);
}
