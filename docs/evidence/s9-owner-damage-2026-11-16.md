# S9: causer requests, owner decides: first live test (2026-11-16 03:00-03:30)

Rule from the AU-RW session (ACU-RE-0092, AVATAR_SPEC D-series): the attacker's machine never writes the victim's health;
it sends a damage request to the owner, only the owner applies it and decides the death, and copies show the owner's value.
Stage 1 here: the host owns all synced guards, and a linked guest guard is the copy (`s3-takeover-release-2026-11-16.md`).

## Code (commits 82e3cd5, 0edc39d; asi 737267cc..., tree clean on both copies)
- **`[Coop] OwnerDamage`**, default follows `FightIntents` (ini overrides), read once. `[Coop] DamageRelay` gets the same default.
- **Guest:**
  - `ApplyGuards` records each linked copy's `CSrvNPCHealth` service with its host id (`OwnerDamageNoteCopy`).
  - A detour on `CSrvNPCHealth::SetLife` 0x19FB4C0 (prologue `55 8B EC 56 8B F1`; it is the only NPC life setter,
    `ops/evidence/character-health-setter.md`) blocks any **lowering** of a copy's life that is not our own relay write
    (`t_odRelay`). It returns the value, as SetLife does, and queues the drop as a fraction of the copy's max.
  - The player frame publishes the running total on `IC_DAMAGE` (6): `u32 total permille, u32 last`.
- **Host:**
  - The intent drain applies the delta scaled to its own max: `SetLife` for a partial hit, `KillGuardEntity` when life
    would reach ≤ 0.
  - The guard is found with `ObjMapFind` (its capture map). First run 13664: `ResolveEntityById(F00036E1)` missed, so the
    engine registry does not index these runtime-minted ids. The same fix went into `FiResolve` for guest → host fight intents.
- The copy then shows the host's health (`ApplyRelayedHealth`), and the host's death reaches it on the existing death lane.

## Live run (two copies via two_copy.ps1, only `FightIntents=1` set; host 33008, guest 13012)
Both at the soldiers (36, -74). The guest attacked the soldier body `3E203F70` (linked to host guard `F0005990`): slot
EnterFight, then the pad sequence (`real_fight.py`).
| step | log line |
|---|---|
| guest: the hit on the copy blocked, request sent | `[OD] tx objId=00000000F0005990 +1028 permille total=1028 ok=1`; stats `blocked=1 passedHeal=3 tx=1` |
| host: owner decides | `[OD] host kill objId=00000000F0005990 ent=3BEE4A30 life 36 -> dead (+1028 permille, killed=1)` |
| host: death sent | `[Kill] push health event objId=4026554768 life=65535 -> peer` (4026554768 = 0xF0005990) |
| guest: copy dies from the host's decision | `[Kill] peer DEATH objId=4026554768 -> ent=3E203F70`; `[Mirror] snapshot death ... applied=1` |
| guest: no second death | `[GKT] push SKIPPED objId=4026554768 ent=3E203F70 reason=already-killed-by-peer` |

**PASS:** one guest attack on a linked guard gave exactly one death, on both machines, decided by the host.

**Also seen in this run:** the guest's fight intents now apply on the host's puppet: `[FI] apply enter ent=3BEE4A30 can=1 do=1`
(the same guard), then exit; a second enter/exit on `F00036F4`. Host FI stats `rx=2/0/2 applied=2/0/2 unresolved=0`.

## Not checked / caveats
- **A partial (non-lethal) hit:** the one blocked hit was lethal (1028 permille), so the host's `SetLife` branch and the
  copy's health display via DamageRelay were not exercised.
- **The guest resolved the host's death** through its position fallback (`[Kill] objId=... unknown -> position
  fallback ent=3E203F70`), not its id table. It chose the right body; why the table missed is not established.
- **Guest kills outside SetLife** (if any engine path kills an NPC without SetLife) would still use the old guest death
  push. None was observed.
- **Damage the host deals to its own guards** is unchanged (the host is the owner).
- **Repeatability:** n = 1.

## Non-lethal hits (2026-11-16 03:40-04:10)
- Real pad fights on the guest landed no hit in three rounds (pids 24256/28060). `passedHeal=13` shows the detour saw only
  life increases (regeneration) and let them through.
- Test command `pilot.py lifehit ENT DMG` (kCmdLifeHit, commit 67e1358): calls the real `CSrvNPCHealth::SetLife(life - DMG)`
  on ENT's health service from the guest's player frame, unmarked, as a combat hit calls it. Only the caller differs from a
  real hit.
- **First attempt (asi 3e15f7fa), two defects:**
  - Rounding: 5 requested, 6 applied per hit (the guest rounded the fraction up and the host rounded the damage up). Host
    36 → 30 → 24 → 18.
  - The copy did not follow: `DamageRelay` was read lazily after the runner's ini restore, and the install ini also had an
    explicit `DamageRelay=0` left from earlier testing.
- **Fixes (commit 605f382, asi 7e7078f7):**
  - the guest rounds to the nearest permille; the host applies `round(total x max / 1000) - already applied`;
  - the FightIntents-following defaults (DamageRelay, BindLock, BindRadiusM) are read at plugin start
    (`[OD] session defaults` line);
  - the install ini's `DamageRelay=0` was changed to 1 (owner's S9 decision; an explicit 0 still turns it off).
- **Result (host 25972, guest 9580, linked copy 38852080 ↔ host guard F0003713, three `lifehit 5`):**
  | hit | guest | host | guest copy |
  |---|---|---|---|
  | 1 | `life 36 -> requested 31 -> now 36` (blocked), `tx +139 permille total=139` | `life 36 -> 31 (max 36)` | `hp relay ... life 36 -> 31 (host 31, local max 36)` |
  | 2 | blocked, total=278 | `31 -> 26` | `31 -> 26` |
  | 3 | blocked, total=417 | `26 -> 21` | `26 -> 21` |
  **PASS:** exact damage, owner-applied, and the copy shows the owner's value after each hit.
- **Still open:**
  - `ApplyRelayedHealth` writes the host's absolute life clamped to the local max. With different maxes (36 vs 30 seen
    earlier) the copy's proportion is off. The host max is not on the wire.
  - A partial hit from real combat was not captured.

## More non-lethal hits, and the max-life question (2026-11-16 afternoon, host 29420 / guest 23296, asi 67a20fa2)
- `pilot.py lifehit ENT 5` on the guest's linked copies:
  - soldiers (max 36), 2 of 4 linked; the other 2 were unlinked bodies, so not blocked;
  - pirates at (110,-72) (max 50), 8 of 8 linked.
- Every blocked hit was applied exactly on the host. For example `[OD] host apply objId=F0006F4B ... life 50 -> 45 (max 50,
  +100 permille)`, and for an already hurt one `F0007113 life 10 -> 5`. The copy then showed the host's value:
  `[Mirror] hp relay ... life 50 -> 45 (host 45 of max-seen 50, local max 50)`.
- **Different max life:** in 10 of 10 linked pairs the max was equal on both sides (36/36, 50/50). Links have
  required the same archetype since the BindLock work, so the earlier 36-vs-30 case may not happen now (not proven).
- The host max is not on the wire. `ApplyRelayedHealth` now keeps the highest Life the host sent per guard as its max,
  and can scale to the local max behind **`[Coop] HpRelayScale`** (default 0). This is a heuristic: a guard first seen
  already hurt reads as full until a higher value arrives. Not exercised live, because no mismatch occurred.
