# Guard-fight pass 2 with FightOrderPuppet=1: still not targeted (2026-11-16)

Same rig, owner-driven. `FightOrderPuppet` flipped live to 1 (`guard_poi.cpp:206`, 2 s poll). Verdict: **FAIL**, and the
failure is one layer earlier than the order layer.

## What moved

The order wrapper installed and reported `fightOrder on=1`, but **`ticks=0 members=0 ordered=0`**: `FightOrderAfterUpdate`
returns before counting unless the FightManager's target record (`fm+0x304`) already resolves to the puppet. It never
did, so no order was ever handed out. `FightOrderPuppet` can only act on a target that already exists - it is a
correction to order, not a target selector.

## Perception exists, in the wrong direction

- `[EvtPup]` = **103** lines, `id=258` about once per second, `recv=38257D60` - the replica emits stimuli and a
  receiver takes them.
- `[Sight]` on the same run: `hostile(G,P)=0`, `puppetPicks=0`, `VIS=0`, `detect=0`, and `puppet=00000000` late in the
  log (the accessor returned null while `[PL]` still printed a pointer, i.e. the puppet was transient around then).
- Player PDP rose again (`FDEpost=60`, `hitPhase 2`, `deBuild 2`, `csDamage 62`), replica PDP stayed `0` with
  `total FDE=1`, life pinned at 125.

## Where the next work is

Target *selection*: something makes a guard keep Edward and never swap to a nearer, stimulus-emitting replica. Candidate
checks, in order: what `sub_161BD30`/`sub_14B6CC0` (the two `sub_13F22E0` callers documented above) require of a
candidate, whether the replica's faction/team words are populated (`facP=facPl=00000000` in every sample), and whether
the fight group ever admits it at all.

Until a guard's fight target is the puppet, guard damage on the replica cannot be measured - the instrument is ready and
proven (`guard-fight-pass1`), the life mirror is in, and the only missing link is selection.
