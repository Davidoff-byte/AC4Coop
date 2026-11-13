# Who switches a mirrored guard's action (S1, 2026-11-13)

Binary: AC4BFSP.exe, md5 2058342866688f780c8b34526a65bc35 (IDA batch, idat 9.3, ref/ida/AC4BFSP.exe.i64).
Question: on the guest, borrowed guard bodies (BhvGenericNPC disabled) still play a different action than the
host 3-12% of moving frames (replay rig, builds 8B1E2110 / 015CAA5E). Which engine paths switch their action?

## Verified in IDA
- **One funnel.** `xrefs_to 0x651C20` = 4, complete: 0x00658CF8 in sub_658CA0, 0x00658E5C in sub_658D80,
  0x006705B5 in sub_670150, 0x0067117B in sub_670D80. Every evaluator switch passes through sub_651C20
  (`int __thiscall sub_651C20(this, Action* a2, int, int, int, int, int, char)`; this = ActionComponent).
- **sub_658CA0** is reached from sub_65DCB0 (0x0065DDA7) and sub_65DC80 (0x0065DC99). `xrefs_to 0x65DCB0` = 5:
  SetTargetAction sub_6625D0 (0x006625F4), sub_6B0090 (0x006B0163), sub_662210, sub_6623B0, sub_666B30.
- **ActionPostUpdate auto-transition.** sub_6695F0 is registered as a frame step in sub_5863D0 (data refs
  0x00587DA3, 0x00587E35). Body: `if (!*(u8*)(ac+0x159)) { ...; return sub_669220(ac); }` (tail jump 0x00669626).
  sub_669220 walks the three controller lists at ac+0x4C (8-byte heads, u16 count at +6, 124-byte items); for each
  evaluator (sub_E10E50) it takes the action queued at ev+0x2A4, and when its condition holds (sub_4C8120 on
  entity+0x5C -> +0x8A0) and the current action ev+0x2A8 is of kind 1, calls `sub_6B0090(ac, *(ev+0x2A4), 0, 0)`.
  A second branch re-enters through sub_648BD0 (0x00669300). This is the clip graph advancing on its own clock.
- **Default-action fallback.** sub_658D80 (called from 0x0066260C; frame-step data ref 0x005870BF) returns at
  once unless global byte_2AC1E9C is set; then, if an action entry fails sub_4BA440, it resets the entries and
  switches to the default action `sub_489910(ac+0x5C)` with flags 3. An error path, not a steady writer.
- **Statechart.** sub_52DF80 is called from the Human update sub_16FA7D0 (0x016FA857) and sub_1C9B7D0
  (0x01C9B7D3), plus two data refs (0x01E4E4D8, 0x026D6C60). It is `sub_52C220(0)`: the AIActor context tracks,
  whose states call SetTargetAction themselves (crash stack 6625F9 -> 65DCB0 -> 651C20, phase1.cpp comment).
- **Vtable.** 0x026E34D8 holds code pointers (0x016A1640, 0x009F0AB0, ...): the BhvGenericNPC vtable the plugin
  uses. 0x026E2820 is the ASCII string "BhvGenericNPC" (bytes 42 68 76 47 ...); RE-NOTES.md:155 is wrong.

## Not yet known (needs the probe run)
Which of these produces the observed mismatches, and how often: (a) ActionPostUpdate auto-transitions,
(b) context-state SetTargetAction calls inside sub_52C220, (c) the fallback, (d) sub_670150 / sub_670D80.
Possible non-writer causes still open: SetTargetAction no-ops when the alias doesn't resolve (sub_489970), and
the probe/apply layer mismatch (ACTLAYER_PRIMARY vs g.layer).

## Probe
`src/action_writer.cpp`, `[Coop] ActionWriterProbe` (default 0, read at load): detour on 0x00651C20 (6 stolen
bytes `55 8B EC 8B 45 08`, checked before patching; trampoline resumes at 0x00651C26). For switches that land on
a body we drive and differ from the host alias (our own GuardAnimApply writes are counted separately), it counts
the return address plus the EBP-chain return addresses above it. Line: `[ActWriter] mirrored switches= toHost=
other= own= | top: count:r0<r1<r2<r3(a= h=)`. Observation only. Build clean, gates pass; not yet run in game.
