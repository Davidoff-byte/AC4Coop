#pragma once
// Guards perceive the co-op puppet on the host. See guard_sight.cpp. [Coop] GuardSeesPuppet (live; the patch is
// installed when the key is present).
namespace GuardSight
{
    void Init();
    // One summary line for the last window (counters reset). Returns 0 when not installed.
    int  FormatReport(char* out, int cap);
    // True when this guard's sight sensor currently tracks the puppet (refreshed within 3 s).
    bool TracksPuppet(const void* guard);
}
