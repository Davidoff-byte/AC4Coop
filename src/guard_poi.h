#pragma once
// Per-guard "player of interest" (host). See guard_poi.cpp.
// [Coop] GuardPlayerOfInterest (live, default 0; hooks installed when the key is present): for a guard whose sight
// sensor tracks the co-op puppet, the engine's per-guard "the player" lookups return the puppet.
// [Coop] ConflictNullGuard (read at load, default 0): the world conflict object skips a target with no +0x94 service.

namespace GuardPoi
{
    void Init();
    int  FormatReport(char* out, int cap);
}
