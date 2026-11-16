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
