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
    // [Coop] GuardPerceivePuppet (default 0, needs GuardSeesPuppet): see ChoosePerceive in guard_sight.cpp.
    bool PerceiveOn();
    // True while this guard's detection target (CRLDetect start) is the puppet, set less than 3 s ago.
    bool DetectsPuppet(const void* guard);
    // Host: per-guard perception for the guest lane. False when the flag is off. stage 0 none, 1 sight visible,
    // 2 detection start; targetIsPuppet 1 = the puppet (the receiver's local player), 0 = the host player.
    bool PerceiveFor(const void* guard, unsigned char* stage, unsigned char* targetIsPuppet);
    // [Perceive] summary line (counters reset); 0 when the flag is off or not installed.
    int  FormatPerceiveReport(char* out, int cap);
}
