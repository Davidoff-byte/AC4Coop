#pragma once
// [Coop] PuppetListener (default 0): report whether the replica carries an EventListenerComponent, which is what the
// event post resolves before any damage handler runs (0x5220E0 -> 0x5210A0 -> class off_279BF68, named
// "EventListenerComponent"). Without it a posted FightDamageEvent is dropped and no counter downstream can move.
// [Coop] PuppetListenerAttach (default 0, needs PuppetListener): attach the missing component (0x6F7180, a2=0
// allocates). Local/cosmetic only - the authoritative outcome is the guest's until R6 (docs/design/guard-fight-sync.md).
namespace PuppetListener
{
    void Init();
}
