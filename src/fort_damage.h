#pragma once

// [Coop] FortDamageTest (default 0, read at first tick): the F1 oracle of docs/design/fort-sync.md 4 as a
// one-shot test harness. It is a TEST, not a sync lane: it damages exactly one tower once and reports what the
// engine did, so "damage -> disabled -> phase advance" stops being [UNPROVEN].
//
// With the flag 0 nothing runs and no game memory is read. With it 1 the tick searches (every 2 s, up to 256
// coordinators) for the first GcLNavalFort coordinator, resolves tower[0], reads the four fields plus the tower's
// health/disabled flag, calls the engine's own ApplyDamage once with hp0 + 1.0f, then re-reads at ~+0.5 s and
// ~+2 s and prints a PASS/FAIL verdict computed from those observations. The only write in the game is whatever
// ApplyDamage itself does; +0x16C and +0x2E3 are never written by this module.
void FortDamageTestTick();
