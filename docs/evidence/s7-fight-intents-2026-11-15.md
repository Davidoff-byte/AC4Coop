# S7 slice 1: fight intents, live on two copies (2026-11-15 22:40-23:40)

Design: `docs/design/s7-fight-intents.md`. Flag `[Coop] FightIntents=1` with `IntentLane=1`, `NativeReplica=0`. Two copies via
`tools/two_copy.ps1 -Interactive` (host top-right, guest bottom-left). Every log line quoted is from `phase1_log_<pid>.txt`.
Verdict: **same-PC two-instance tested**: enter/exit 3/3, one real player attack replicated end to end. Not two-machine tested.

## Builds (each `[Build]` line checked on both copies)
| commit | asi (prefix) | change |
|---|---|---|
| 0ab42a4 | deba4e8c | slice 1 |
| 7072bcb | eb77d98f | flag checks the lane on every call (it was cached as off before Net::Init) |
| 9578551 / 93156c5 | c719e786 / 068f2267 | diagnostics |
| a27658d | 9c24463b | canonical id = `HandleContainer::m_ObjectID` of `[E+0xC8]` |
| 6fa1b31 | 69202484 | one op per puppet frame |
| 6df30dc | b05fe0a9 | unresolved target retried for up to 2 s |

## Runs
1. **Owner's real fight (14160/31720, eb77d98f).**
   - Host `tx enter`, 4x `tx attack`, `tx exit`, at the right path changes (163 / 173 / back to Move).
   - Guest `rx` seq 1-6 in order (first `base=1`).
   - **But every message had `target=0`**, so nothing was applied (`applied=0/0/0`).
2. **Owner's second fight (8880/14484, c719e786).** 2 enter / 7 attack / 2 exit sent and received, still target 0.
   - No `[FI] diag:` line was logged, which means the focus handle **matched** a character and the id was 0.
3. **Self-run diag (20956, 068f2267).** Slot EnterFight on the host's Edward against a soldier:
   `[FI] diag match: ent=3B754B00 handle=3895EC58 canonical=0 ownObjId=0 entHost=1 role=1`.
   - The focus chain finds the right soldier, but `ManagedObject::GetObjectID` on the entity returns 0.
   - Fix: take the id from the handle's HandleContainer (`m_ObjectID`, the entity lane's `es.objId`, phase1.cpp:5382),
     checked by `hc->m_Object == E`.
4. **Fixed id (32488/5852, 9c24463b).**
   - Host `tx enter target=00000000F00064F8` (diag `hcObjId=F00064F8`).
   - Guest `rx enter`, `apply enter ent=3A0AA970 can=1 do=1`, so the puppet entered the fight.
   - The exit arrived in the same drain batch and was tested against the stale path, so it was skipped and the puppet
     stayed in fight. Fixed by applying one op per puppet frame.
5. **Enter/exit x3 (21888/23360, 69202484): PASS.**
   - Host 3x `tx enter target=00000000F000650C` + `tx exit`.
   - Guest 3x `apply enter ent=3A07FF40 can=1 do=1` (path before 1/2/3/4/5/6), then
     3x `apply exit can=1 do=01426101` (path before 1/2/163/166/167/168).
   - Stats `rx=3/0/3 applied=3/0/3 unresolved=0`; puppet ends in Move (getter 0x27, path 1/2/3/4/5/6).
6. **Real player attack (same pair): PASS, n = 1.**
   - Pad sequence X, X, X+stick, RB, X next to the soldier (`attack_try.py`) gave host path 1/2/163/166/173/204, fight stage 61.
   - Host `tx enter` + `tx attack target=00000000F000650C` then `tx exit`.
   - Guest `apply enter ent=3A07FF40 can=1 do=1`, then `apply attack ... do=00000004` (PerformCombo, puppet path before
     1/2/163/166/167/168), then `apply exit can=1`.
   - Then a second soldier joined (`tx enter target=00000000F000A45D`): guest `unresolved`.
7. **Unresolved again with the 2 s retry (30248/19328, b05fe0a9).** Slot EnterFight on a not-yet-hostile soldier: host
   `tx enter target=00000000F001098A`, guest `unresolved` after 2 s, so enter and exit were not applied.
   - Pad X did not start a fight in two scripted attempts here.

## What this shows
- **Proven:**
  - sender detection (owner's fights: enter, attack and exit at the right path changes);
  - target identification (focus chain → HandleContainer id);
  - reliable in-order delivery on the intention lane;
  - the receiver drives the replica's own FSM with the right target, 3/3 enter/exit plus one real attack;
  - action copy suspended while the puppet is in fight (`puppetFight` flag; the alias diag showed the FSM's own fight
    actions in the probes).
- **Identity is the limit (S3):** a target resolves only when the guest's entity lane has bound that guard
  (`GuestObjFind`). With `SyncEnemiesOnly=1` a soldier that was not yet hostile is not synced. The engine-registry fallback
  (`GuestObjFindResolve`) also missed, consistent with `F00xxxxx` ids being minted per machine (UNVERIFIED in IDA).
- **Owner's eye report (run 2):** "I saw one guard attacking the puppet". That is guard AI on the host, not this slice;
  noted, not analysed.

## Not done / open
- Scripted pad attacks are unreliable (1 of 4 attempts started a fight). The owner's real fights are the better driver.
- Damage and hits (does the puppet's combo hurt the guard on the guest; what the host sees): S9.
- More than one opponent, counters, kills, assassinations; Attack 154.
- A negative control run with `FightIntents=0` this session (no `[FI]` lines expected; the default-0 build had none).
