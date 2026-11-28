#pragma once

// World-state sync (docs/design/world-state-sync.md). Three independent lanes, each behind its own [Coop] flag
// (default 0; flag 0 = no game memory read, no byte patched):
//   TimeOfDaySync     host publishes TimeOfDayManager hours on IC_COORD (CF_TOD_HOURS); the guest adopts them
//                     through the engine setter 0x0040D0C0 (vtable slot +0x6C). Paused is never touched.
//   CollectableProbe  read-only: logs every live GcLCollectable (entity id, +0x60/+0x6C/+0x70, position) once.
//   CollectableSync   modes 1/2 are NEED-RE in this slice (no proven hide path); every mode behaves as 0.
//   CoordTriggerRelay host-side detour of the statechart trigger routine 0x00A014A0 (filtered to 4 coordinator
//                     classes), relayed on IC_COORD (CF_TRIGGER); the guest resolves and LOGS only (no fire).
// Evidence: ops/evidence/world-state-tod-collectables-coord-2026-11-27.md.

#include <cstdint>

namespace WorldSync
{
    // DLL load: reads the flags; installs the 0x00A014A0 detour only when [Coop] CoordTriggerRelay != 0.
    void Init();
    // Per player frame (game thread), next to CoordSyncTick().
    void Tick();
    // True when one of these lanes needs the intent lane drained (FightIntentDrain gate).
    bool DrainNeeded();
    // One IC_COORD message from the lane's single consumer. True = consumed (field 19 or 21, which belong to
    // this file); false = not ours, hand it to CoordSyncApplyMsg.
    bool OnCoordMsg(uint64_t wireId, uint32_t seq, uint8_t flags, const uint8_t* payload, uint8_t len);
}
