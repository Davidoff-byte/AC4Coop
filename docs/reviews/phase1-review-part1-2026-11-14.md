# phase1.cpp review, part 1 (lines 1-6000), 2026-11-14

Scope: `src/phase1.cpp` lines 1-6000 at `feat/launcher` 420b0d3 (12,113 lines total; lines 6001-end are a
separate review). Correctness only: crashes, races, wrong offsets against cited receipts, truncating
buffers, default-flag mismatches, logic that contradicts its own comment. No style. No `src/` change.

## What was and was not verified

- Every line from 1 to 6004 was read (table below). Context lines outside the range were read only where
  a finding depends on them (callers, the frame-hook SEH wrapper, the wire structs).
- **No binary check was possible in this session.** `AC4_GAME_DIR` / `AC4_KNOWN_EXE` are unset, the
  test install (`G:\`) is not mounted, and no IDA session was available to this reviewer. Every claim
  about engine behaviour below therefore rests on the file's own comments, its own measured evidence
  notes, or the repo's `decomp/units.json`. Items that need the binary or a live run are marked
  **UNVERIFIED**.
- Nothing was built or run.

## Severity counts

| severity | count | of which UNVERIFIED |
|---|---|---|
| crash | 3 | 1 |
| wrong behaviour | 8 | 3 |
| minor | 8 | 1 |
| **total** | **19** | **5** |

## Findings

### F1. Puppet action writes race the puppet's own Human update (crash)

`phase1.cpp:1688-1717` (comment and toggle), `1879-1907` (`SetTargetAction`), `1913-1983` (blend-in
descriptor write), `1995-2015` (blend factors / time scale).

```cpp
// Exactly ONE action-writer at all times so two threads never play an action at once (the crash):
...
g_switchCd = 4;   // ~4 frames of no-action-writes around the toggle
...
ac->SetTargetAction((int)s.actionAlias, 0, 0, (int)s.actionChannel, (int)s.actionLayer);
```

Why it is wrong: the premise is refuted by the same file.
- `5840-5843` (measured 2026-11-12, build 5D862E45): the player's Human update, where `ApplyRemoteState`
  runs, executed on 16 worker threads, and "every write we make onto another body (guard transform,
  puppet matrix) is made from a different Human's update, at an arbitrary point relative to that body's
  own update".
- `5742-5744`: the body's own Human statechart "keeps running although BhvGenericNPC is disabled", so
  disabling the behaviour component does not remove the second action writer.
- `104-105`: crashctx 7792_1 (build cd4e6808) has the stack "through SetTargetAction 0x006625F9 called from
  the puppet action switch" - the puppet path has already crashed in this way.

A 4-frame cooldown serialises nothing across threads. The mirrored-guard path got the fix
(`MirrorActionsDeferred`, `5731-5817`); the puppet's `SetTargetAction`, the `ev+0x1C8..0x1DC` / `ev+0xE4`
stores and the persistence/blend writes did not. The re-validation at `1949-1950` is a TOCTOU check
against a concurrent writer and cannot close the window.

Failure: peer leaves the ground context (any parkour/climb); our `SetTargetAction` or blend store lands
while the puppet's own statechart is inside its action update on another thread; freed evaluator
(`+0x1F4`) or `0x006855D1` class AV in `AC4BFSP.exe`. The SEH in `OnFrame` does not cover the engine's
own thread.

Fix (minimal): queue the puppet's action request (alias, channel, layer, ratio, blend descriptor, blend
factors, time scale) and issue it from `OwnDrain` when the hook fires for the puppet's Human, as
`MirrorActQueue`/`MirrorActDrain` do for guards; keep the direct path behind a flag for A/B. Also correct
the "Main-thread only" comment on `DespawnPuppet` (`2048`).

### F2. `ToggleVirtualPad` reads engine state from the poll thread without SEH (crash)

`phase1.cpp:427-452`; caller `dllmain.cpp:385-389`.

```cpp
World* world = World::GetCurrentWorld();
Entity* player = world ? world->GetPlayer() : nullptr;
if (player && readable(player, 4)) {
    s_p0Start[0] = player->m_GlobalMatrix.M41;
```

Why it is wrong: `V` is bound unconditionally in `PollThread` and calls this directly (not through a
request flag like F9-F12). `GetPlayer` (0x70E810, `World.h:201`) walks the world's entity manager while
the game mutates it on other threads. There is no `__try` here or in `PollThread`; an AV on a non-game
thread terminates the process. `dllmain.cpp:233-249` (`sampleOnce`) already wraps the identical
world -> player read in SEH for exactly this reason. `readable(player, 4)` covers 4 bytes, not
`m_GlobalMatrix`.

Failure: player presses V (a common key) during a load, world change or death; stale `player` -> AV on
the poll thread -> game exits.

Fix: turn it into a request (`g_vpadToggleReq = 1`) consumed in `OnFrameInner`, or wrap the body in
`__try/__except`; gate the key on `VPadEnable`.

### F3. `MirrorActDrain` reads a slot after releasing it (wrong behaviour, race; flag default 0)

`phase1.cpp:5800-5806` against `5773-5791`.

```cpp
if (InterlockedCompareExchange(&s_maq[i].human, 0, key) != key) continue;
InterlockedDecrement(&s_maqCount);
const uint32_t alias = s_maq[i].alias;
const unsigned int ent = s_maq[i].ent; const uint64_t objId = s_maq[i].objId;
if (GetTickCount64() - s_maq[i].ms > 1000) ...
```

Why it is wrong: the CAS publishes the slot as free (`human == 0`). `MirrorActQueue`, running at the same
time in the player's update on another thread (`5840-5843`), treats `cur == 0` as a free slot
(`5778-5779`) and writes `alias/ent/objId/ms` for a different Human before the drain has read them.
`ms` is also a non-atomic 64-bit read on x86.

Failure: with `MirrorActionsDeferred=1`, guard A plays guard B's clip (`SetTargetAction` with a foreign
alias), or a fresh request is dropped as expired.

Fix: copy `alias/ent/objId/ms` into locals while `human == key`, then CAS; discard the copy when the CAS
fails (the writer only writes fields into slots it saw as 0 or took back by CAS itself).

### F4. Hostility engine calls run with `HostileOnly=0` (wrong behaviour, contradicts its comment)

`phase1.cpp:4794` against `3903-3904`.

```cpp
const int hostile = (int)HostilityToPlayer(player, e);   // every candidate, every capture
```

Why it is wrong: the comment says "calling into it has never run in a game, so it stays opt-in until a
live run". The call is unconditional; only the rejection at `4800` is gated. So `0x017114C0` and two
`0x0133A4C0` calls run per candidate per capture in the default configuration. Both units are
`UNVERIFIED-EVIDENCE` in `decomp/units.json`. AGENTS.md: new behaviour ships behind a flag that defaults
to 0. SEH stops an AV, not an engine-side lock or a side effect.

Fix: `const int hostile = (HostileOnly() || DiagEnabled()) ? (int)HostilityToPlayer(player, e) : HOST_UNKNOWN;`
(or update the comment if a live run has since cleared it, citing the run).

### F5. `bok` is computed and then ignored (wrong behaviour)

`phase1.cpp:1685`, `1747-1770`.

```cpp
bool bok = readable((void*)qB, 0x150);
...
if (locoNow) {   // feed HumanGroundData ...
qB->m_GroundData.m_CurSpeedRatio = s.curSpeed;
```

Why it is wrong: `bok` is only printed (`2040`). A null or unreadable bundle (a puppet whose deferred
`AddToWorld` has not completed) faults on the first store; `ApplyRemoteState`'s `__except` (`2041`)
swallows it, so context replication, CAS activation and action replication for that frame are skipped,
every frame, with one log line per 30 exceptions.

Fix: `if (locoNow && bok) {`.

### F6. `s_hits` byte counters read and written as 32-bit (minor; `ShipDumpFloats` default 0)

`phase1.cpp:3194`, `3240-3242`.

```cpp
static unsigned char s_hits[kSlots][0x1000 / 4];
unsigned& h = *(unsigned*)&s_hits[slot][o >> 2];
if (h >= (unsigned)kPerOff) continue;
++h;
```

Why it is wrong: each counter is one byte, but `h` spans four. The `>= 3` test also sees the next three
offsets' counters, so an offset goes silent as soon as a neighbour has logged once. For
`slot == 319, o >> 2 >= 1021` the access runs up to 3 bytes past the end of `s_hits` (the store writes back
the zero bytes it read, so the overrun is an out-of-bounds read and an equal-value write).

Fix: `unsigned char& h = s_hits[slot][o >> 2];`.

### F7. "Ring buffer" eviction is `ptr % 512` (wrong behaviour)

`phase1.cpp:5269-5273` (also `5288`, `5349` with `% 256`); comment `5257-5261`.

```cpp
int slot = (int)((unsigned int)e % (unsigned int)MIRROR_TABLE);   // deterministic eviction
```

Why it is wrong: the comment promises "evicting the oldest". Entity pointers are aligned, so `e % 512`
takes only a few low bits: at 16-byte alignment only 32 of the 512 slots can ever be evicted into
(UNVERIFIED: the engine's entity alignment; any alignment >= 4 bytes leaves at most 128 slots). Once the
table is full, two dead bodies whose addresses share those bits evict each other, `MirrorKilledLocally()`
goes false for one of them, and the visual-death path re-runs (the freeze `5257-5259` describes).
`DeathAppliedRecently` (`5338-5351`) uses the same `% 256` scheme.

Fix: a real cursor, `table[s_next++ % MIRROR_TABLE] = e;` (one static cursor per table).

### F8. `MirrorAliasShouldApply` saturates for the process lifetime (wrong behaviour)

`phase1.cpp:5366-5376`.

```cpp
if (slot < 0) {
    if (n >= MIRROR_TABLE) return false;
```

Why it is wrong: the table is function-static, never evicted, never reset. After 512 distinct entity
addresses it answers false for every new body, so the host's clip is never applied to a newly mirrored
guard (non-`GuardAnimFull` path). This is the same silent-saturation shape the file fixed as B-1
(`3718-3724`) and B-2 (`3817-3819`).

Fix: evict the least recently refreshed entry (stamp array as in `GuestObjAdd`), or reset with the other
per-scene tables.

### F9. Vanish lane can report a false death through a recycled health service (wrong behaviour, UNVERIFIED)

`phase1.cpp:4899-4964`, validator `3534-3543`, reset `11444-11449`.

```cpp
if (!HostHpSvcValid(w.hpSvc)) { w.hpSvc = 0; continue; }
...
life = *(unsigned short*)(w.hpSvc + 0x5C);
...
if ((life == 0xFFFFu || dead) && w.seen && w.lastLife >= 1 && w.lastLife <= 2000) {
    ... PushHostDeath(w.objId, w.lastEnt, vanishPos);
```

Why it is wrong: a watch whose guard left `m_Entities` is re-read every capture until it is tombstoned or
the scene key changes. `HostHpSvcValid` proves "a CSrvNPCHealth with a plausible Max", not "this guard's
service". `3528-3532` says the engine recycles these objects. If the block is reused for another NPC
inside the same scene key and that NPC dies, the old objId is pushed as dead and the guest kills the
wrong mirror.

UNVERIFIED: whether services are recycled inside one scene key (the comment only claims it for scene
changes).

Fix: retire a watch after N captures unseen (or when `ResolveEntityById(w.objId)` is null), and drop
`hpSvc` when the remembered entity's vtable/id no longer matches.

### F10. `HostLifeFor` refuses new watches at 256 (minor)

`phase1.cpp:3668-3671`.

```cpp
if (s_hostLifeN >= 256) return nullptr;
```

Why it is wrong: no eviction and no log. Within one scene key, the 257th distinct guard is never
watched, so its death never reaches the guest. The reset on a scene-key change (`11449`) bounds it.
Same shape as B-1.

Fix: evict a tombstoned or longest-unseen watch, and log once when full.

### F11. Proxy "structural hint" reads `+0x146` on the GcLShip, not on the entity (wrong behaviour, UNVERIFIED)

`phase1.cpp:3044-3050` and comment `2949`; compare `2321-2336`.

```cpp
void* ho = ShipGcLObj(se);
if (ho && readable((char*)ho + 0x140, 0x0A)) {
    __try { hint = (int)*(uint16_t*)((char*)ho + 0x146); hintOk = true; }
```

Why it is wrong: the file locates the sub-entity array on the ship **entity** (`+0x140`, u16 count
`+0x146`, "copied by FUN_00503600", `2321-2336`), and `1129-1131` stresses that the entity and the
GcLShip are different objects. `ShipGcLObj` only proves 0xD4 readable bytes. No receipt in `docs/` or
`decomp/` puts a sub-entity count at GcLShip+0x146 (searched for `0x146`). If the field is on the entity,
tier 3 of the selection ranks ships on unrelated data.

Fix: read `*(uint16_t*)((char*)se + 0x146)` after verifying it against the binary, or drop tier 3.

### F12. Proxy ids truncated to 32 bits, then resolved as 64-bit (minor, UNVERIFIED)

`phase1.cpp:1218`, `1270`, `2973`, `3007`, `3024`.

```cpp
struct ProxySrcPair { uint32_t srcId; uint32_t ghostId; };
if (ResolveEntityById(g_proxySrcRing[k].ghostId)) { ++live; continue; }
```

Why it is wrong: `ResolveEntityById` passes `hi = 0` and then requires `GetObjectID() == id` as a 64-bit
value (`5446`). For any id with a non-zero high dword, `ProxySrcPrune` treats a live (hidden) duplicate
as gone and frees its pair, so its source can be duplicated again.

UNVERIFIED: whether ship ids exceed 32 bits (`5592` shows minted guard ids in `0xF0000000..0xF7FFFFFF`;
`5505-5509` distinguishes "authored" ids, whose width is not documented here).

Fix: store `uint64_t` ids.

### F13. `reloadBuilder` gives an asynchronous loader the address of a stack local and sleeps on the frame thread (crash, UNVERIFIED)

`phase1.cpp:294-314`; called at `579-585` (`ReloadBuilder` defaults to 1).

```cpp
HandleContainer* handle = hm->CreateReference(id, nullptr);
char ok = LoadingThread::AddLoadTask_Callback(0, stream, 0xFFFFFFFF, id, (int)&handle, 0, 0, 0, 0, 0);
...
for (int i = 0; i < 200; ++i) { ... Sleep(10); }
return nullptr;
```

Why it is wrong: `handlePtr` is `&handle`, a local of this frame. If the loader keeps the pointer and the
task finishes after the 2 s timeout, it writes into a dead stack slot. Independently, the loop blocks the
player's Human update for up to 2 s per attempt (net mode retries every 120 frames, `11538`). If
completion needs the game's frame to advance, the loop always times out.

UNVERIFIED: what `0x...AddLoadTask_Callback` does with `handlePtr` (no receipt; the header only names it
`handlePtr`).

Fix: pass a static or heap `HandleContainer*` slot that outlives the call, and poll across frames instead
of sleeping.

### F14. Two comments disagree on the slot 0x12C calling convention (minor)

`phase1.cpp:1079-1081` against `1140-1143`.

```cpp
// WRITE through slot 0x12C, the __stdcall thunk to sub_1225A10 ... __stdcall is the thunk's own
// convention (this on the stack), which is also how the engine's virtual call site passes it.
```

Why it is wrong: `1140-1143` quotes the thunk (`push ecx; movss [esp],xmm0; call sub_1225A10; retn 4`)
and states `this` arrives in ECX, i.e. `__thiscall`, and the code uses `__thiscall` (`1153`). The quoted
bytes support the code. The header comment is stale and invites a "fix" to `__stdcall` that would
break the call.

Fix: delete the `__stdcall` sentences in `1079-1081`.

### F15. `CaptureLocalState` leaves 128+ bytes of the wire struct uninitialised (minor)

`phase1.cpp:1350-1559` (`ctx` is only written in the swim branch `1486-1518`); caller
`phase1.cpp:11323` (`PlayerState local;`); struct `netstate.h:32-130`.

Why it is wrong: `PlayerState` has no initialisers apart from the HookMath members. When
`contextId != CTX_SWIM`, `ctx` (128 B) and the padding go out as stack contents every packet. The receiver
ignores `ctx` outside swim, so behaviour is unaffected, but process stack bytes are sent to the peer,
and wire captures are not byte-reproducible.

Fix: `memset(&s, 0, sizeof(s));` at the top of `CaptureLocalState`, before the field writes.

### F16. Corpse budget comment versus code (minor)

`phase1.cpp:4321-4332`, `4385`.

Why it is wrong: (a) "cap corpse records per snapshot at CORPSE_BUDGET_MAX ... oldest-first": the cap
admits corpses in emit order (nearest to the peer first, `4856-4886`), not oldest first. (b) "A chunk
holds ENTITY_CHUNK_DATA / 52 = 26 records" assumes 52-byte records; a corpse that carries `EF_ANIM` adds
`GUARD_ANIM_WIRE_FIXED + 2*nBlend` (`net.cpp:345-347`), so 24 corpses can exceed one 1368-byte chunk,
which is what the budget was meant to prevent.

Fix: correct the comment, or size the cap from the record bytes the encoder reports.

### F17. `TrackMirrorDeathForRelay` watch table never evicts (minor)

`phase1.cpp:5189-5221`.

```cpp
if (s_mirrLifeN >= 64) { GktSkip(..., "mirror-watch-table-full"); return; }
```

Why it is wrong: 64 entries, no reset, no eviction; `GktSkip` logs at most 10 lines for the whole process
(`5032`). After 64 mirror addresses this relay is silently off. `5000-5010` says the `[GKT]` guest watch
supersedes this lane, which bounds the impact.

Fix: LRU eviction as in `MarkPeerApplied`, or remove the lane if it is superseded.

### F18. `L()` lazy log open is not thread-safe (minor)

`phase1.cpp:191-216`.

Why it is wrong: `L()` is called from the poll thread (`ToggleVirtualPad`), from the XInput hook
(`VPadGetState`) and from game worker threads (`5840`). If two first calls overlap, both run `_fsopen(...,
_SH_DENYWR)`. The loser gets null and may overwrite `f` after the winner set it; the winner's handle
leaks while still denying writes, so every later open also fails and the log is lost for the process.
The window is only the first call.

Fix: open once under `InitOnceExecuteOnce`, or open in `InstallFrameHook` before any other thread logs.

### F19. Render-override targets are rewritten in place while the hook may read them (wrong behaviour, UNVERIFIED)

`phase1.cpp:3478-3485` (writer); reader `phase1.cpp:11804-11813` (part 2's range). The same applies to
`g_ghostSyncPos/Quat` (`1591-1597`, read at `11785-11789`).

```cpp
SyncTarget& t = g_syncTargets[count];
t.ent = (uint32_t)ent;
t.pos[0] = es.pos[0]; ...
...
g_syncCount = count;
```

Why it is wrong: `ApplyShips` overwrites entries `0..count-1` while `g_syncCount` still holds the
previous packet's count, with no lock. If `SetGlobalMatrixInternal` is called on another thread while
this runs, the hook can match an entry whose `ent` is new and whose `pos/quat` are old (or half
written), and render that ship for one frame with another ship's transform. A torn ghost quaternion
gives a one-frame shear.

UNVERIFIED: which threads call `SetGlobalMatrixInternal` (`5840` shows that Human updates, which move
entities, run on many worker threads).

Fix: double-buffer `g_syncTargets` and publish the buffer index plus count with one interlocked store;
copy the ghost pose under an SRW lock as `s_pupM` already does (`6455-6466`).

## Checked and found consistent (no finding)

- Blend-node offsets: node = ev+0xE0; node+0xFC = ev+0x1DC; node+0x104 = ev+0x1E4; ev+0x1F4 =
  node+0x114 (`99-102`, `1953-1955`, `5733-5734`) all add up. The `0x2C0` read span covers +0x2BC (`1916-1918`).
- `BlendTypeSafe` mapping (`1955-1963`) matches its comment (`98-107`): 2->1, 4->3, 5 skipped.
- Player-marker vtable arithmetic (`2201-2246`): 0x26A270C-0x0C+0x70 = 0x26A2770 = 0x26A277C-0x0C, as stated.
- Vtable bound checks for slots 0x1B4/0x12C/0x130 (`1103`, `1152`, `1182`) cover the 4 bytes read.
- `ReadShipClass` gate (`1136`) agrees with `EntityKindRole` bit layout (`3866-3868`).
- Ini defaults of globals declared in range match their comments (load site `12014-12061`).
- Frame-path SEH: `OnFrame` wraps `OnFrameInner`, `OwnDrain` and `MirrorActDrain` (`10862-10880`), so
  unguarded engine calls inside the frame path (`ShipGcLObj`'s 0x01038330, the T39 block in
  `CaptureShips` `3126-3160`, `ApplyRemoteState`'s pre-`__try` ship block `1590-1662`) are caught there.
  The cost is that the rest of that frame's work is skipped; that is not listed as a separate finding.
- `blendCount` from the wire is clamped on receive (`net.cpp:1502`). `actionLayer`/`actionChannel`
  (`netstate.h:70-75`) are not range-checked before `GetCurrentAction`/`SetTargetAction` (`1884`, `1890`).
  Whether the engine bounds-checks the layer is UNVERIFIED, so this is noted here, not as a finding.

## Ranges read

| range | read | notes |
|---|---|---|
| 1-400 | full | globals, `L()`, `readable`, handle helpers, VPad |
| 400-799 | full | VPad install, pad probe, skeleton attach, `TrySpawnPuppet`, crowd probe start |
| 800-1199 | full | crowd probe, scene key, ghost/ship HP helpers |
| 1200-1599 | full | proxy ring, hull identity, `CaptureLocalState`, `ApplyRemoteState` start |
| 1600-1999 | full | `ApplyRemoteState` (wire clamps, ownership, context, swim, CAS, actions, blend fix) |
| 2000-2399 | full | blend factors, `DespawnPuppet`, ship ctor probe, Havok body, marker suppress, `DuplicateShip` |
| 2400-2799 | full | `SpawnGhostShip`, `TryShipDupProbe`, `DespawnGhostShip` |
| 2800-3199 | full | naval list, `BoardableTestTick`, `SelectProxySource`, `CaptureShips` start |
| 3200-3599 | full | ship delta watch, `ApplyShips`, health cache, obj maps |
| 3600-3999 | full | death lanes, guest obj table, kind/role, hostility, scope counters |
| 4000-4399 | full | slots, histograms, flags, StageLog, corpse budget |
| 4400-4799 | full | `CaptureGuardAnim`, `EmitGuardEntity`, `CaptureGuards` walk |
| 4800-5199 | full | `CaptureGuards` emit + vanish lane, kill relay, GKT tables |
| 5200-5599 | full | mirror tables, resolver, `ApplyRelayedHealth` |
| 5600-6004 | full | wide bind, deferred mirror actions, own-update flags, foot IK probes |
| outside range | targeted | `phase1.cpp` 6440-6570, 10860-10895, 11318-11326, 11436-11460, 11535-11640, 11770-11830, ini reads 12014-12061; `dllmain.cpp` 230-250, 261, 340-392, 623-626; `net.cpp` 185-232, 280-370, 1490-1510; `netstate.h` 5, 20-135, 188-305; `HookMath.h` 1-70; `Scimitar/LoadingThread.h`; `decomp/units.json` (cited units) |
