#pragma once
// [Coop] PlayerDamageProbe (default 0, read at load): read-only counters along a guard hit's damage chain for the puppet
// and the local player. [Coop] PlayerDamageRelay (default follows FightIntents): the event-post hook (H1) that hands a
// guard's hit on the puppet to the puppet's owner (phase1.cpp). Both in player_damage_probe.cpp.
namespace PlayerDamageProbe
{
    void Init();
    int  FormatReport(char* out, int cap);
    bool RelayOn();   // [Coop] PlayerDamageRelay as read at load (default follows FightIntents)   // the [PDP] window lines (counters reset); 0 when not installed
}
