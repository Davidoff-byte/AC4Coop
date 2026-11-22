#pragma once

// [Coop] GroundTickProbe (default 0): R1 of ops/evidence/guard-ai/brain-design-2026-11-22.md.
// Question: does the Human Ground statechart tick on a guard copy the mod has bound and disabled on the guest?
//
// The Ground context (vtable 0x026E8AE8, = *(Human+0x720)) is updated through vtable slot 15 only:
//   0x026E8B24 -> sub_1D80950 (thiscall ctx, 1 arg): lazily allocates the 0xA80 helper at ctx+0xA0, then
//   dispatches sub_1D65BB0 on it (active-state count u16 helper+0x11E, ids u16 *(helper+0x118)).
//   xrefs_to 0x1D80950 = one data ref, the slot itself (IDA job cc/r1).
// The probe replaces that one vtable dword with a counting thunk that calls the original unchanged, and keys the
// count by ctx. Read-only otherwise: no field written, no other code patched. The slot dword is verified to be
// 0x01D80950 before the swap; anything else and the probe stays off.
namespace GroundTick {
    void Init();                                      // reads the flag, installs the counter
    // role: 1 = guest bound guard copy, 2 = host native guard, 3 = puppet, 4 = local player.
    // ctx = *(Human+0x720). Called on the game thread; also emits the report every 2 s.
    void Note(void* entity, void* ctx, int role);
    bool On();
}
