# [Coop] ForcePuppetFight - enlisting the replica by hand (2026-11-16)

Status: **built, not run**. Default 0 (`[Coop] ForcePuppetFight`), read each tick, host only, 2 s cadence.

## Why

Passes 1-3 and the branch map (`busy-gate-branches-2026-11-16.md`) show the replica is perceived
(`VIS=9`, `hostile=1`, picked 65x) but never joins a fight group (`pupInGroup=0`, `enlisted=0`), so no order is ever
issued and no damage can land. `sub_161BD30` orders only members with `+0x2C == 1` (`fightmanager.md:149-153`).

The enlist path is `sub_1625590(fm, a, b)` - "start the fight between a and b": it removes the old record, calls
`sub_1625330` to create the fight, posts status 1 (`sub_1620350`) and cross-SetTargets both sides
(`sub_156CB00`). `fm` is `*(guard+0x64)`; `sub_1397450` reads the same field.

## What it does

Every 2 s, host only: walk `world->m_EntityManager.m_Entities`, pick the nearest *soldier* (Behaviour component
vtable `0x026E34D8`, type word `0x35F7` - the same test `ProbeDeathDiff` uses) within 25 m of the replica that has a
FightManager, then call `sub_1625590(fm, guard, pup)` and log `[PFF] force guard=.. fm=.. pup=.. d=..`.

## Why it should survive the next tick

`sub_15DABC0(fm, rec)` gives up on a record only while the target's Team is outside `fm+0x30C`. If the replica's team
word is 0 and 0 is not in that map, the forced record is cleared and this patch shows `[PFF]` lines with no lasting
effect - which is itself the answer to the Team question. The probe now logs `ai+0xA4` (the `ResolveFaction` source,
`0x133A4C0`) for both bodies so that is checkable in the same run.

## UNVERIFIED

- That calling `sub_1625590` from our thread mid-combat is safe (it allocates and removes records).
- That a guard so enlisted actually *attacks* a body with no attack chain; if it circles and whiffs, `FightOrderPuppet`
  / order 2 work (C in the branch note) is the next step, not this.
- Team validity as above.

## Pass 4 criterion (pre-registered)

PASS = `[PoiFO] pupInGroup=1` on a later sample and a `[PDP] puppet` stage counter moving. FAIL = `[PFF]` lines present
but `pupInGroup` still 0 (team/validity), or `[PFF]` absent (scan/call fault).

## Enlist-flag follow-up (added same night, static)

sub_15E3110 is the function that writes the enlist flag ([a2+0x2C] = 1, 0x015E31C3) - but its ONLY reference is a
vtable slot (data ref 0x026DA880), so nothing ever calls it for the replica. [PFF] therefore sets ec+0x2C = 1
itself for every record in m+0x30C whose +0x40 is the replica, and logs enlisted=<n>. That is the gate
sub_161BD30 reads; without it a created record is never ordered.

UNVERIFIED: that a hand-set flag survives the next sub_15E3110/sub_15DABC0 pass, and that order 2 alone makes a
guard attack a body with no attack chain.
