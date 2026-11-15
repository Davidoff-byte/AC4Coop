# S3 first look: out-of-combat guard identity for fight intents (2026-11-16 00:25-00:55)

Question (owner): sync guards out of combat too? Do AU-RW's recent commits help? Context: `s7-fight-intents-2026-11-15.md`
run 7, where a fight target on a soldier that was not yet hostile stayed `unresolved` on the guest.

## AU-RW (fetched 2026-11-16; newest coop work on `origin/acu-85-coop-re-plan`, ACU-RE-0072..0087)
- `d879ffc` "universal wire-ID rule" (ACU-RE-0075): **message-type** ids, i.e. CRC of `"zenitar_" + type + "_ID"`, 963/963
  against a 0/300 control. It is not entity identity.
- `e20b520` (ACU-RE-0076) item 4: an incoming `NetEntityObjectDescriptor` binds to the local entity whose `+0x10` id equals
  the descriptor's `+0x110` (`sub_141C5B2D0`). "Static world entities (NPCs, props) are matched by entity ID, not
  spawned." This is the lead for AC4: name a guard by something both machines load from the same data.

## AC4 facts used
- A soldier's own `ManagedObject::GetObjectID` (0x9F0B50, non-minting) is **0**; its HandleContainer id is
  `F000xxxx` (run 20956 diag). That matches the minted-band finding in `guard-spawner-id-2026-11-13.md` (branch
  `claude/optimistic-satoshi-0a0b33`, `cae66f8`): spawned guards have no authored id.
- That file's candidate key is the SceneSpawner that spawned the guard (`S+0xCC` = the guard's HandleContainer), keyed by
  `(ownerId, spawnerId)`.

## Probe 1: `[Coop] SpawnerIdProbe` (commits `7807217`, `4c0df4d`; asi 10ef58b5...): NEGATIVE for this route
- **METHOD:** every entity in `World::m_EntityManager.m_Entities`; components at `+0x60`, count u16 `+0x66`; match component
  vtable `0x026900B8` (SceneSpawningComponent). Run on both copies of a two-copy pair, in the soldier area, three passes
  60 s apart.
- **POSITIVE CONTROL:** the walk reached 3361 entities and 10927-10953 components (pids 14028 / 2656), so it can return
  components.
- **RESULT:** spawnerComps=0 on both copies in every pass (5-7 soldiers loaded, guardsKeyed=0).
- **COVERED:** `m_Entities` only. **NAMES:** SceneSpawningComponent, vtable 0x026900B8.
- So the spawners' layout entities are not in `m_Entities`. Per `guard-spawner-id` section 6.1(b), the next step is
  static: find the SceneSpawningComponent registry, starting from the callers of slot 36 (0x10462B0). Not done.

## Finding 2: soldiers ARE synced out of combat. The guest's 1.5 m body match was the gap
- The host's entity lane already sends out-of-combat soldiers:
  `[Sync] scope candidates=154 inRadius=154 hostile=154 ... sentPerCap=7.00 (SyncEnemiesOnly=1, ..., HostileOnly=0)`
  (pid 14028). `SyncEnemiesOnly=1` is a soldier/civilian test on `entity+0x9C`, not hostility (phase1.cpp comment at
  the scope block).
- Guest with the default `BindRadiusM=0` (pid 2656): `mirror ok=44 nobod=101`. Most host guard records find no guest body
  within 1.5 m, so `GuestObjFind` has no entry and the fight target stays unresolved.

## Test: `[Coop] BindRadiusM=12` (existing wide bind), two copies (25184 / 1600), asi 10ef58b5...
- `[WideBind] R=12 runs=2 need=2 exact=0 wide=1 left(noArchetype=1 ...)`; guest `mirror ok=161 nobod=0 steal=0`.
- Slot EnterFight on the host's Edward against three **out-of-combat** soldiers (4.5 m, 15.9 m, 16.4 m away):
  - host `tx enter` targets F00036D9 / F000569D / F00056B3;
  - guest `apply enter ent=3B6677F0 / 25A30120 / 3DB43BB0 can=1 do=1`, then `apply exit can=1` each;
  - stats `rx=3/0/3 applied=3/0/3 unresolved=0`.
- **PASS 3/3**, against 0/1 at `BindRadiusM=0` (s7 run 7).
- **Caveats:**
  - The wide bind re-reads the ini live, so it dropped to 0 when `two_copy.ps1` restored the ini
    (`[WideBind] BindRadiusM=0 (was 12)`). The test ran on bindings made in its first two runs, which are sticky. A long
    run needs the setting kept.
  - The wide bind gives each host guard the **nearest** guest NPC of the same archetype within 12 m: a proximity identity,
    not a true one. The guest body is then mirrored to the host guard's position by the entity lane, so the puppet faces
    the right spot. Whether the same individual is chosen every time is not checked.

## Recommendation
- Short term: set `[Coop] BindRadiusM=12` with `FightIntents=1`. It is an existing flag; it fixes out-of-combat targets in
  this test.
- Proper S3: a real cross-machine key for spawned guards (spawner registry, static next step above), so binding no longer
  depends on proximity.
