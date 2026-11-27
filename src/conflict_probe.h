#pragma once
// [Coop] ConflictProbe (default 0, read at load): logs who calls the FightManager's conflict starter (sub_1625330) and
// attack API (sub_1620140) for the player vs the puppet, with return addresses. Observation only. See conflict_probe.cpp.
namespace ConflictProbe
{
    void Init();
}
