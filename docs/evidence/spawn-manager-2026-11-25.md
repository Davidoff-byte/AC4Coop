# The spawn manager for everything - map and live census (2026-11-25)

Question (owner): is there one spawn manager for everything, so the guest can stop spawning on its own and build only
what the host spawned, driven by the host's data?
Binary: AC4BFSP.exe md5 2058342866688f780c8b34526a65bc35 (SP only; AC4BFMP.exe not opened - every negative here covers SP).
Static: ida_batch / IDAPython on `ref/ida/AC4BFSP.exe.i64` (DBFL_KILL). Live: build C9FCF1DC (`[Coop] SpawnCensus=1`,
`src/spawn_census.cpp`), host pid 20272, one route Dry Tortuga -> Havana -> sea (-4368, 2443), ~9 min. Logs in ignored
`local/census-20261125/`. Prior work re-read first: `ops/evidence/guard-ai/brain-identity-spawn-rng.md` (request entry,
job graph, removal queue, RNG) and `docs/evidence/naval-s0-2026-11-25.md` "The ship spawner".

## 1. The machinery (static)
| Stage | VA | What | Evidence |
|---|---|---|---|
| Create request | `dword_2AC1E88->vt[0x84/4](ctx, params)` | the spawn system singleton's request factory; every submitter goes through it | decompile `sub_569560`, `sub_56F8E0` (loops a params array), CrowdHerder `sub_1978B50` (prior) |
| Set the source | `sub_79C6C0(req-source, a4, a5)` | source / position of the request | callers: 0x569590, 0x56F92C, 0x1300502, 0x167D576, 0x195464D, 0x195F1F3 |
| Submit | `0x0079B8D0` SpawningRequest::Spawn | `req->vt[0]()` gate, then `req->vt[3]()->vt[0x88/4](req)` | prior (p14), 11 callers |
| Run (per frame) | job `sub_455310` -> client `sub_5C5770` -> `sub_5B06E0` -> `sub_45E330` | lock + `params->vt[0x7C/4]` | naval-s0 "The ship spawner" |
| Build (funnel) | `0x0046D8E0` SpawnEntityParams::Spawn base (vt `0x01E43BB0` slot 31) | builder `[params+0x4C]`->`vt[0x44/4]`(&out, params+0x3C) | xrefs_to = 4, complete: 0x1128778 SpawnObjectParams, 0x1898E2E SpawnNPCParams/SpawnCharacterParams, 0x1996ABB SpawnShipParams (+SpawnPlayerShipParams via 0x122BD30), data 0x1E43C2C |
| Add / remove (manager side) | `sub_422FF0` -> `sub_5A8A80` (ret 0x423007); `sub_423010` -> `sub_5A8AC0` (ret 0x423020) | shared virtual add/remove slot (`sub_5B1E90` / `sub_5B1EB0`, referenced from several vtables) | owner.py xrefs |

Slot 31 of every SpawnParams class (class map vtables): SpawnEntityParams 0x46D8E0, SpawnObjectParams 0x1128740,
SpawnNPCParams = SpawnCharacterParams 0x1898E20, SpawnShipParams 0x1996AB0, SpawnPlayerShipParams 0x122BD30,
SpawnPlayerParams 0x1877970 and SpawnInventoryParams 0x1598AF0 (these two are NOT callers of 0x46D8E0).

## 2. Live census (host, 1785 world adds, 1499 removes, 443 requests, 172 funnel builds)
Every add into `sub_5A8A80` classified by: built by the funnel (entity seen leaving 0x46D8E0, ring of 4096) and
character (`AIEntityHelper::GetHuman != 0`). Pre-registered claim: "one manager for everything" holds if >= 95 % of
character adds and >= 95 % of ship adds came out of the funnel.

| Kind | Adds | From the funnel | Share |
|---|---|---|---|
| Characters | 141 | 132 (SpawnNPCParams 0x026FD610) | **93.6 %** - just under the bar |
| Ships | 21 | 21 (SpawnShipParams 18, SpawnPlayerShipParams 3) | **100 %** |
| Non-character, non-ship | 1623 | 19 (SpawnNPCParams that are not humans: animals / props, UNVERIFIED which) | ~1 % |

The 9 characters outside the funnel: 7 through the shared add slot (0x423007, the same one the manager uses; built by a
params class that does not reach 0x46D8E0 - SpawnPlayerParams / SpawnInventoryParams are the candidates, UNVERIFIED) and
2 through `0x00C0F4F5` (inside an undefined band; 2 adds = one per world load, consistent with the player body,
UNVERIFIED). Verdict: **the manager makes every ship and ~94 % of characters; it is not the source of most world
entities** - those are local cosmetics:

| Caller of the add | Adds | Owner (method: data/code xrefs; class names by nearest class-map vtable, UNVERIFIED) |
|---|---|---|
| 0x490C59 `sub_490C20` | 900 | FXSpawnedEntity slot (effects) |
| 0x16E9B73 `sub_16E9510` | 241 | object built by `sub_1701C20` (primary vt 0x026EBBB0, not in the class map) |
| 0x94397B (undefined band) | 134 | loop adding `[esi+0x1E]` children, `sub_942680` |
| 0x10720F8 / 0x107203C `sub_1072050` / `sub_1071FB0` | 74 | naval code (`sub_10922E0`, `sub_108D060`) |
| 0x718366 / 0x7183B2 `sub_718300` | 128 | `sub_71D010` |
| 0x11228F4 `sub_1122850` | 30 | `sub_112EB50` <- `sub_11394C0` |
| 0x16D3A50 / 0x16D3D24 | 31 | NavalCrewPropData / NavalCrewMultiplePropData (crew props) |
| 0x5455CB `sub_545540` | 27 | FXSpawnedBallistic |
| 0x6AE35C, 0x73C2AC, 0x6D06CD, 0x1080CF7, 0x12D24D3, others | 26 | GameplayCoordinatorWorldComponent, camera/menu, GcLBoardingBattle (2), misc |

Requests: 443, two request vtables (0x01E6D77C: 430, 0x01E4D74C: 13); submitters 0x56F938 (`sub_56F8E0`, 339, reached from
naval code `sub_108F310`, `sub_1090580`, `sub_1120E50`), 0xFCDB33 (`sub_FCDB00`, 54, statechart states and naval),
SpawnerComponent 0x6B2422 (18), CrowdHerder 0x1978C9A (18), others 11. Requests exceed builds (172): declined, budgeted or
reused - not decoded.

## 3. What this means for "guest does not spawn, host spawns, host drives"
- **One gate is enough for actors:** every AI ship and nearly every NPC is built by 0x46D8E0 out of a request submitted at
  0x79B8D0. FX, props and crew props are outside it and are local cosmetics; they need no sync.
- **The guest must spawn through the request path, not by calling Spawn + AddEntity** (3/3 crashes, naval-s0): build the
  request with the factory (`dword_2AC1E88->vt[0x84/4](ctx, params)`), set its source (`sub_79C6C0`), submit (0x79B8D0), so
  the manager's post-spawn step runs.
- **Portable spawn key:** the params object is a local pointer. The host must send what identifies it on the guest:
  params class, the builder's ObjectID (authored, outside the minted band, LIVE-PRIOR `00000006'12CECF74`), the selector
  seed, and the transform after the add. UNVERIFIED that the builder ids are equal on two machines (not measured).
- **Identity:** engine ids are minted per process and cannot be rewritten (prior); keep hostId -> local entity, as the
  guard lane does.
Design and open questions: `docs/design/host-spawn-authority.md`.

## 4. Replaying a request, and the portable key (2026-11-25, builds 3CAF4CC5 / 8B2DD07F / 60981F22)
Static: the whole submit is one cdecl function `sub_569560(item, owner, ctx, id_lo, id_hi)` (disasm 0x569560..0x5695A0):
`req = dword_2AC1E88->vt[0x84/4](ctx, item)` (`sub_F72ED0`, thiscall, retn 8), `sub_79C6C0(req, owner, id_lo, id_hi)`
(thiscall, retn 0Ch; req+0xC = owner, req+0x18/+0x1C = id; id 0 -> taken from `owner->vt[0x48/4]`), `0x79B8D0(req)`.
| Test (pre-registered) | Result |
|---|---|
| K: the request item's ObjectID is a portable key (>= 90 % authored and shared by both copies) | **FAIL**: 144 + 147 requests, every item is a SpawningSpecification (vt 0x01E4A278) with ObjectID **0**; request ids high half 0 (minted) |
| Who requests | owner vt 0x026BDE50 = GameplayCoordinator (vt 0x026BDEE0) + 0x38: **129/144**; 0x02708E80: 9; 0x01E60988: 4; 0x026E11F8: 1; only 2 distinct ctx values per copy |
| R: re-submit a captured request through `sub_569560` (host, newest request, captured 93 s earlier) | **PASS (n=1)**: submitted=1, the manager built a SpawnNPCParams human (`[Replay] build after fire`), its manager add count went 57 -> 58, host alive after |
| K2: the GameplayCoordinator's ObjectID is authored | **FAIL (n=1)**: `00000000:56FDD448` - high half 0, minted |
**Defect found and fixed (mine):** the K2 probe called HandleContainer_GetObjectByID `0xA1F160` on that minted id from
inside `__try`; it raised C0000005 while holding the engine object lock `0x4DD601C`, the handler swallowed it, the lock
stayed held and **both copies deadlocked at boot** (hang dump `D:\AC4-RE\dumps\hang-18148-20261125-130944.dmp`: three
game threads in RtlEnterCriticalSection(0x4DD6020)). Build 60981F22 removes every by-id lookup from the probe.
Rule: never call an engine function that takes a lock under `__try`.
**Consequence:** no id identifies a spawn on both machines (spec 0, request minted, coordinator minted). The portable key
has to be built from what both machines share: the requester's world position + its spec index + the params class.
## 5. A spawner key both machines share (2026-11-25, builds DB2EB1FE .. 794A4B48, differential on two copies)
Method: both copies boot the same save; each request's spawn spec (SpawningSpecification, vt 0x01E4A278), its requester
and the spec's sub-objects are dumped with lock-free reads (`[KD]`, `[K3]` lines); `tools/oneoff/spawn_key_diff.py`
compares the copies. Request ORDER at boot was identical (148/148, 151/151) in two runs and off by one in a third, so
order is not a key.
- spec+0x28 -> SpawnPositionStaticParams (vt 0x01E5F428): local position at +0x10 (also +0x40), anchor handle at +0x30.
  Static: `sub_69D090` (local -> world through the anchor entity's matrix at e+0x10, or `sub_6BC3A0(*(e+8))` when e has
  a parent), called from slot 22 `sub_6A92E0`.
- Predicted world spawn point W = local x anchor matrix (no engine call). Result by anchor class (run 794A4B48, as sets):

| Anchor | Requests | Equal on both copies | Unique |
|---|---|---|---|
| Entity (vt 0x01E4CE90, static) | 69 / 68 | **68 W points (99 %)** | yes (max reuse 1) |
| EntityGroup (vt 0x01E4A128 = ships) | 71 / 71 | all 71 **local** offsets; W differs (the ships themselves are placed at random) | 32 offsets, reused up to 6x (same crew layout per ship class) |
| none (no position params) | 10 / 10 | owner classes equal; 9 are owner 0x02708E80 with sea-level world points that differ per copy (random sea traffic) | - |

Key, therefore: static anchor -> W on a 0.5 m grid; ship anchor -> (ship's sync identity, local offset); none -> the host
must say where (random).

## 6. [Coop] SpawnShare slice 1 (static anchors) - built, runs, but has almost nothing to hold (builds 94D7B552 .. 545F3ADB)
Host publishes each static key on intent channel 15 (IC_SPAWN, wireId = key); the guest holds its own matching request
inside the radius and replays it through sub_569560 when the key arrives.

| Run | Result |
|---|---|
| 23695D42 | host published 67 keys at boot; the guest **received all 67 on connect** (the lane's join replay = the connect snapshot). Every guest request passed as "host position unknown": all were made while loading |
| 529BF92B (hold while loading too) | **FAIL, guest froze at boot** (frame 10 for 6 min, 1 request): a held boot request stalls the loader; the tick that would release it does not run while loading |
| 545F3ADB (hold only in world, `Phase1_InSave`) | both alive through boot, travel to Havana and 50 s running into town; host published 104 keys, guest received 106; guest static requests: **90/90 during loading, 0 in world**, so nothing was held |

**Verdict:** for static-anchored spawns the two games already spawn the same NPC at the same point from the same data
(section 5: 99 %), and they do it during loads, when holding is unsafe. Holding them is the wrong tool. What they need is
the **binding**: guest entity <-> host entity by key (the entity spawned at W), replacing today's nearest-body binding.
What genuinely diverges and needs the host: ship-anchored crews (ships placed at random) and position-less random
traffic. SpawnShare stays default 0.
