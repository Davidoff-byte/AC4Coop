#pragma once
// Host test: raise the stimuli the player's gunfire posts (ids 48 + 49), with the co-op puppet as the source, so
// guards react to the puppet. See puppet_stim.cpp and docs/evidence/player-stimuli-2026-11-13.md.
// [Coop] PuppetStimSeq: fires once each time its value changes (live, re-read every second; the first value read
// only arms it). [Coop] PuppetStimMode: 1 = template to the puppet's own entity (engine broadcasts it), 2 = one
// stimulus per NPC within the id's radius (our copy of the broadcaster). [Coop] PuppetStimIds: default "48,49".

namespace PuppetStim
{
    // Called from the puppet's own Human update on the host (phase1 OwnDrain, puppet branch).
    void OnPuppetUpdate(void* puppetEntity);
    // [Coop] FightSkipPuppet (live; hook installed when the key is present): the host's fight system gives the
    // puppet no orders (sub_13F22E0 takes its no-AI exit for it).
    void Init();
}
