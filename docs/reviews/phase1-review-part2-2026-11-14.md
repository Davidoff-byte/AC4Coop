# Correctness review, part 2: phase1.cpp 6001-12113, net.cpp, deref_guard.cpp (2026-11-14)

Read-only review of `src/` at `420b0d3` (branch `feat/launcher`; reviewed in worktree `wt-review-b`,
branch `docs/review-phase1-b`). No `src/` file was changed. Scope: correctness only (crashes, races,
stale pointers, SEH gaps, naked-asm stack/flag breakage, offsets against receipts, wire codec
mismatches, truncation, flag defaults, comment-vs-code contradictions). Style is out of scope.

**Evidence basis.** Every finding below was checked against the source at `420b0d3`. No IDA session
and no game executable were available to this review (`AC4_GAME_DIR` unset; `G:\Games\...` absent),
so any claim about engine bytes rests only on receipts already in the repo (`decomp/units.json`,
`decomp/candidates/*.cpp`, `docs/archive/ops/experiments/E1-identity.md`) and is marked
**UNVERIFIED** where those receipts do not settle it. Nothing here was run or live-tested.

Severity: **crash** (process can fault / memory corruption), **wrong** (wrong behaviour, no fault),
**minor** (diagnostic, latent, bounded, or comment drift).

## Ranges read

| File | Lines read in full | Context lines read to check findings |
|---|---|---|
| `src/phase1.cpp` | 6001-12113 | 225-262, 1231, 1342, 2045-2072, 2813, 3388, 4045, 4267-4315, 4461-4610, 4983-4997, 5038-5050, 5155-5183, 5300-5335, 5536-5570, 5600-5612, 5815-5929 |
| `src/net.cpp` | 1-2045 (all) | - |
| `src/deref_guard.cpp` | 1-1140 (all) | `src/deref_guard.h` (all), `src/Hook.h`, `src/Hook.cpp` 12-27, `src/dllmain.cpp` 540-580 |
| headers | `src/netstate.h` 200-305, `src/relay_proto.h` 14-78, `src/stun.cpp` 27-40 | `decomp/units.json` entries 009F0B50, 00528200, 00744CF0, 010EF8C0, 019FB4C0, 00636480, 0133F550; `decomp/candidates/009F0B50.cpp`, `00744CF0.cpp`; `docs/archive/ops/experiments/E1-identity.md` 8-30; `docs/ops/evidence/RUN-2026-10-29-frozen/phase1_log_27376_head+tail.txt`; `docs/logs/phase1_log_16208.txt` |

## Counts

| Severity | Count | Of which UNVERIFIED |
|---|---|---|
| crash | 5 | 2 |
| wrong behaviour | 19 | 8 |
| minor | 27 | 3 |
| UNVERIFIED risk (unclassifiable without disassembly) | 2 | 2 |

## Top 5

1. **phase1.cpp:10570, crash.** `IdentityIdOf` passes a stack argument to `0x9F0B50`, which ends in a
   plain `retn` (C1). This leaks 4 bytes of stack per call, and the live log already shows the damage.
2. **phase1.cpp:11497-11506, crash.** The "puppet entity gone" branch never calls `PuppetOwnClear()`
   (C2). `OwnDrain` and `Phase1_PuppetEntity()` keep using the freed puppet.
3. **phase1.cpp:11340-11381, wrong.** `DrainRemote` returns 0 on every frame without a packet, so the
   peer counts as absent on those frames (W-P3-1). Puppet drive and interpolation then run only on
   frames that received a packet.
4. **net.cpp:566-569 + 588-589, wrong.** When the first packet of an epoch (or a body's first record)
   is lost, the receiver rebuilds that body's `hp/arch/stage/anim` from a zeroed shadow (N1).
   `arch = 0` switches off the class check in guest binding (phase1.cpp:6925/6941/6887).
5. **phase1.cpp:9256-9282, wrong.** A refused guest kill re-enables the mirror's behaviour and never
   disables it again (W-P2-1). The playback dummy then runs local AI for the rest of the session.

---

## A. Crash

### C1. phase1.cpp:10570: stack imbalance calling `GetObjectID` (0x9F0B50)

```cpp
return Hook::CallClassMethod<uint64_t, void*>(0x9F0B50, obj, 0u);
```

- **Why it is wrong:** `CallClassMethod<Ret, C, Args...>` (Hook.h:22-26) deduces `Args = unsigned`,
  so the cast type is `__thiscall(void*, unsigned)`. The caller pushes 4 bytes and expects the callee
  to pop them.
  - The thunk takes no stack arguments. `E1-identity.md:16-21` disassembles it as
    `push 1 / push 0 / push ecx / mov ecx,... / call sub_A200D0 / retn`, a plain `retn`.
  - `decomp/candidates/009F0B50.cpp:2` (`sub_9F0B50(void *this)`) and `src/Scimitar/ManagedObject.cpp:6`
    call it with no extra argument.
  - So every call leaves ESP 4 bytes low. In this SEH frame the callee-saved registers are popped
    relative to ESP before `mov esp,ebp`, so they come back wrong.
- **Evidence it happens:** `RUN-2026-10-29-frozen/phase1_log_27376_head+tail.txt` shows
  `[IB] -1199949239 ent=0xCA0BB9FE ...`: a garbage row index and pointer, and every `[IB]` block ends
  in `EXCEPTION 0xC0000005`.
  - The comment at 10558-10559 ("called with arg_4 = 0, which gates the minting path") is also wrong:
    the thunk hard-codes `allowWrite = 0` itself.
  - `CombatTestKill` (10806) also goes through `IdentityIdOf`.
- **Failure:** register corruption in `IdentityIdOf`'s callers. The loop counter and the entity
  pointer are clobbered, followed by an access violation, as in the log.
- **Fix:** `Hook::CallClassMethod<uint64_t, void*>(0x9F0B50, obj)`, and correct the comment.
- **Confidence:** confirmed against the repo's own disassembly receipt plus a live log. The bytes at
  0x9F0B5F were not re-read in IDA by this review.

### C2. phase1.cpp:11497-11506: freed puppet is still published to `OwnDrain` and to the probes

```cpp
if (!puppetAlive) {                          // entity was freed (world change / teardown)
    L("[Net] puppet entity gone -> drop ref\n");
    g_puppet = nullptr; g_blockReg = nullptr; g_puppetWorldId = 0; g_puppetVtbl = nullptr;
```

- **Why it is wrong:** `DespawnPuppet` (2053) calls `PuppetOwnClear()`, which clears `s_pupHumanKey`
  and `s_pupEnt` (6555-6559). This branch does not call it.
- **Failure:**
  1. `OwnDrain` runs on any Human-update thread (6562). It matches `s_pupHumanKey` (6565) for any new
     Human allocated at the old address.
  2. It then calls `PuppetStim::OnPuppetUpdate(e)` on the freed `e` (6578) and, with
     `OwnUpdatePuppet=1`, `SetGlobalMatrixInternal` (6576). `readable(e, 0x80)` does not detect
     reused memory.
  3. `Phase1_PuppetEntity()` (6467-6472) keeps returning `s_pupEnt` to fight_probe.cpp:44,
     guard_poi.cpp:59, guard_sight.cpp:145 and event_probe.cpp:133.
- **Fix:** call `PuppetOwnClear();` in this branch.

### C3. phase1.cpp:8753 / 8758: `WbTrap` re-arm races the disarm ([Coop] WatchDeadBit=1 only)

```cpp
if (g_wbArmed) WbApplyGuard();               // TF does not retry, so this cannot re-fault
...
if (!g_wbArmed || !g_wbSvc) return EXCEPTION_CONTINUE_SEARCH;
```

- **Why it is wrong:** the check and the `VirtualProtect(PAGE_GUARD)` are not atomic. Disarm restores
  the protection and then clears `g_wbArmed` (8717-8724). Disarm can run on another thread: the
  main-thread toggle at 8837, or another job thread inside `WbTrap` at 8770 or 8809.
- **Interleaving:**
  1. Thread A reads `g_wbArmed == 1`.
  2. Thread B disarms.
  3. Thread A re-applies `PAGE_GUARD`.
  4. The next touch of the page raises `STATUS_GUARD_PAGE_VIOLATION`, and 8758 returns
     `CONTINUE_SEARCH` because the flag is now 0.
  5. The exception is unhandled. Whether an engine frame catches it is UNVERIFIED.
- **Related:** 8873 `for (int i = 0; i < 8; ++i) g_wbTfTid[i] = 0;` on re-arm wipes slots that,
  per the comment at 8733-8735, only their owner thread may release. A thread caught between its
  fault and its single step then loses its slot, and its single step goes unhandled at 8751.
- **Fix:** at 8758, return `EXCEPTION_CONTINUE_EXECUTION` when the fault page equals `g_wbPage`, even
  when disarmed (disarm never zeroes `g_wbPage`). Re-check `g_wbArmed` after `WbApplyGuard`. Do not
  clear the TF slots in Arm.
- **Scope:** the flag defaults to 0 (11013-11014), so only the probe configuration is affected.

### C4. phase1.cpp:11830-11837 / 11806: `SgmDetour` passes per-call state through globals (UNVERIFIED interleaving)

```cpp
mov eax, [esp+0x18]      // ECX = this (Entity*)
mov g_sgmThis, eax
mov eax, [esp+0x24]      // arg1 = matrix* (Transform*)
mov g_sgmMatrix, eax
lea eax, [esp+0x28]      // &arg2 = the updatePhysics bool
mov g_sgmPhysPtr, eax
```

- **Why it is wrong:** the PUSHAD offsets are correct (ECX at +0x18, LESSONS §7). But the matrix
  pointer and the address of the caller's stack slot go into process globals that
  `LogSetGlobalMatrix` reads after the `call`.
  - The note at 5840-5843 records Human updates on up to 16 worker threads.
  - `OwnDrain` itself calls `SetGlobalMatrixInternal` from those threads (6576).
- **Failure:** two threads interleave.
  - Thread X writes `m[0..14]` into thread Y's matrix.
  - `*(uint32_t*)g_sgmPhysPtr = 0` (11806) stores into thread Y's stack, possibly after Y has returned.
  - In addition:
    - `LogSetGlobalMatrix` has no `__try`.
    - Only +0x30..+0x3B is probed before `m[0..14]` is written.
    - `g_syncTargets` / `g_syncCount` race with `EntitySyncTick`.
    - `s_vt[s_n++]` (11775) has no visible bound check before the write.
- **Fix:** push `this`, `matrix` and `&arg2` as stack arguments to `LogSetGlobalMatrix` (no globals),
  and wrap its body in `__try`.
- **UNVERIFIED:** that two `SetGlobalMatrixInternal` calls actually overlap in a live run.

### C5. phase1.cpp:7107 / 7111 / 7158 / 7170: other guards mutated from the player-update thread (UNVERIFIED)

```cpp
((Component*)bestBhv)->SetEnabled(false);
...
((Entity*)best)->SetGlobalMatrixInternal(&m, bigJump);
...
ac->SetTargetAction((int)es.actionAlias, 0, 0, 0xFFFF, ACTLAYER_PRIMARY);
```

- **Why it is a risk:** with the defaults `OwnUpdateGuards=0` and `MirrorActionsDeferred=0`,
  `ApplyGuards` changes guard entities while those guards may be updating on other worker threads.
  The overlap probe at 6737-6748 exists to measure exactly that.
  - The comments contradict each other on the thread: 7385 says main thread, 7467 says worker threads.
  - Changing a guard's action stack while it updates is a plausible crash cause.
- **Fix:** route these writes through the owning-thread drain (`OwnDrain`), as `OwnUpdateGuards=1`
  does, or gate them on the overlap probe reading zero.
- **UNVERIFIED:** needs non-zero `[Own] overlap/driven` counts or a crash dump.

---

## B. Wrong behaviour

### phase1.cpp 6001-8040

**W-P1-1. 6205 / 6261-6266 / 7149: own-body table never shrinks; a full table freezes new guards (OwnUpdateGuards=1)**

```cpp
if (i < 0 && s_ownN < OWN_MAX) {
...
const int i = OwnFind((unsigned int)h);
if (i >= 0) { ... starved = now - last > 300000; }
...
const bool ownWrites = bh && !s_selfTestMode && OwnGuardsOn() && !OwnStarved(bh);
```

- **Why:** `OwnForget` is called only at 7113 (dead or killed bodies). The slot releases at 6859,
  6868, 6979 and 7204 never call it, so `s_own` fills at `OWN_MAX` (128).
- **Failure:**
  1. `OwnPublish` stores nothing for a new guard.
  2. `OwnStarved` returns false for an unknown body, so `ownWrites` is true and the legacy write is
     skipped.
  3. `OwnDrain` cannot find the body (6585-6588), so nothing moves it.
  4. The `[Own] bodies=` field (6762) sits at 128.
- **Fix:** `OwnStarved` returns true when `i < 0`. Evict entries on slot release and when the table is
  full (for example `lastPubUs` older than 1 s).

**W-P1-2. 7532 + 7200-7226: mirrors stay disabled after the host stops sending**

```cpp
} else if (haveIn) {
```

- **Why:** the stale-slot eviction and the wide-bind hand-back live inside `ApplyGuards`, which runs
  only while a snapshot is held.
  - `GetEntities` expires the snapshot after 3 s (net.cpp:1947-1955), which is sooner than
    `SYNC_SLOT_STALE_MS` (10 s, 4045).
  - Nothing else undoes `SetEnabled(false)` (7107) or `m_DriveEntity = false` (7111).
- **Failure:** the host quits, crashes or changes world, and every mirrored guard stays a statue (the
  comment at 7205-7208 names this outcome).
- **Fix:** run the slot-eviction loop on the guest every slow tick, whether or not `haveIn`.

**W-P1-3. 6220-6228 / 6716 / 6730: after a >5 m jump, physics is written at the old position (GuardInterpBuffer=1, OwnUpdateGuards=1, OwnPhysicsMs=0)**

```cpp
if (dx * dx + dy * dy + dz * dz > 25.0f) { ... o.snap = true;
...
if (o.snap) s_own[j].snap = false;
```

- **Why:** `snap` is set when the packet arrives. `OwnDrain` samples the ring about 120 ms in the
  past, and the jump rule (6154-6155) returns the pre-jump `a->pos`. So the first frame writes
  `phys = true` at the old position and clears `snap`, and the real step is written about 120 ms
  later with `phys = false`.
- **Failure:** after a teleport the physics capsule stays behind.
- **Fix:** let `OwnRingSample` report that it stepped across a jump, and decide `phys` from that.

**W-P1-4. 6711 / 6728-6729: StuckProbe and the O1 probes never count with OwnUpdateGuards=0**

```cpp
if (!OwnGuardsOn()) return;
...
s_own[j].ga.misSinceUs = o.ga.misSinceUs; s_own[j].ga.stuckLogUs = o.ga.stuckLogUs;
```

- **Why:** `o` is a local copy (6586). The timer write-back sits after the early return, although the
  comment at 6727 says these timers must persist "when GuardAnimFull is off".
- **Failure:**
  - `misSinceUs` is reset every frame (6644), so the 1 s gate (6646) never passes.
  - With the buffer on, `aliasSinceUs` is also reset every frame (6597-6598), so the gates at 6634
    and 6698 never pass.
  - The probes read zero, which looks like a measured negative result.
- **Fix:** move the write-back above `if (!OwnGuardsOn()) return;`.

**W-P1-5. 6097 / 6190: interpolation ring keeps the old host's clock after a host change (UNVERIFIED reachability)**

```cpp
if (o.ringN && hostUs <= o.ring[o.ringHead].hostUs) return;   // same or older snapshot: nothing new
```

- **Why:** `s_hostBaseSet` is latched once per process, and rings are never cleared. `hostMs` is the
  host machine's own QPC time in ms (3393).
- **Failure:** the guest rejoins a different host machine without restarting. New samples compare
  older than the stored ones and are dropped, so the body holds stale samples.
- **Fix:** reset the time base and the rings on a peer or epoch change, or when a sample is far older
  than the ring head.

**W-P1-6. 6283 / 6291 / 6294: unchecked host `layer`/`channel` reach the engine; the seek targets the wrong layer (UNVERIFIED)**

```cpp
void* cur = ac->GetCurrentAction(&mir, 0, g.layer);
ac->SetTargetAction((int)o.alias, 0, 0, (int)g.channel, (int)g.layer);
ac->SetCurrentActionRatio(hostRatio, 0, false, false, ACTLAYER_PRIMARY);
```

- **Why:** `layer` and `channel` are copied off the wire with no range check (net.cpp:601-602). The
  seek uses layer 0 while the switch uses `g.layer`.
- **Failure:**
  - An out-of-range layer that writes out of bounds without faulting is not caught by the SEH at 6670.
  - A non-zero host layer seeks the wrong layer.
- **Fix:** clamp `layer` to the engine's layer count, and seek `g.layer`.
- **UNVERIFIED:** the engine's layer limit; whether non-zero layers occur.

### phase1.cpp 8041-10080

**W-P2-1. 9256-9260 + 9274-9282: a refused guest kill leaves the mirror's behaviour enabled for good**

```cpp
((Component*)bhv)->SetEnabled(true);
... if (readable(cc, 0x600)) cc->m_DriveEntity = true;   // clear the tick's disable
...
if (!engineDead) { ... return false;
```

- **Why:** every caller marks the body killed locally first (9370, 7065, 5551). The mirror tick skips
  its re-disable and its drive for locally-dead bodies (7102-7111, 7124).
  - If `FindHealthInAI` returns 0 (9018) or SetLife is refused, the function returns with the
    behaviour enabled and does not arm the latch.
  - Retries up to `CLAIM_MAX_ATTEMPTS` re-enable it each time, and then the claim goes terminal.
- **Failure:** the playback dummy runs local AI for the rest of the session. The comment at 7045-7049
  says "the body is no longer touched".
- **Fix:** on every return after the pre-state, restore `SetEnabled(false)` and `m_DriveEntity = false`.

**W-P2-2. 9376: host death path has no already-dead or duplicate gate (crash risk UNVERIFIED)**

```cpp
if (g_entHost) { applied = KillGuardEntity((Entity*)ent); }
else if (!DeathAppliedRecently(ent)) {
```

- **Why:** the guest branch gates on `DeathAppliedRecently`, and `GuestKillMirrorEntity` skips bodies
  that are already dead (9236). The host runs SetLife(-1), the death clip and the collision write
  every time.
  - 7493-7495 claims both lanes collapse duplicates. That is false for the host.
  - 8906-8907 records that a peer kill reaches the apply path more than once.
  - 9232-9234 documents a double SetLife(-1) as the 0x67B2EA crash path (seen on the guest).
- **Fix:** skip when `DeathAppliedRecently(ent)` or when the body is already dead (life 0xFFFF or flag
  0x01000000); otherwise call `MarkDeathApplied`.

**W-P2-3. phase1.cpp:8929 and 5168 (senders) against net.cpp:656-667: kill-push seq from `GetTickCount` can capture the stale marker**

```cpp
out.seq = GetTickCount();
```

- **Why:** net.cpp:654-655 assumes a kill-push seq is far outside `STALE_WINDOW` and never moves the
  marker. Once host uptime is at least 2^31 ms (24.9 days, wrapping at 49.7), `SeqDiff(tick, snapSeq)`
  is at or below -4096, so `SnapshotDone` moves `g_rxDoneSeq` to the tick value.
  - Snapshots (`++g_entSeq`, phase1.cpp:3388) then have d > 4096: they pass but never move the marker
    back, so proto-14 stale filtering is off for the rest of the session.
  - A second kill push in the same tick has d == 0, gets `DEC_STALE`, and the death is lost; the host
    tombstone (8918-8922) prevents a resend.
  - The guest push at 5168 has the same shape.
- **How checked:** I verified the net side of this by hand against net.cpp:658-666.
- **Fix:** give kill pushes their own lane or a header flag that bypasses the stale check and
  `SnapshotDone`.

**W-P2-4. 9048-9049: non-atomic read-modify-write of a component flags dword (UNVERIFIED)**

```cpp
unsigned int before = *(unsigned int*)(svc + 4);
*(unsigned int*)(svc + 4) = before | 0x1000u;
```

- **Why:** Component.cpp:3 says the engine sets bits in this dword with interlocked operations (via
  0x7130C0). A concurrent job-thread update can lose a bit.
- **Fix:** `InterlockedOr((volatile LONG*)(svc + 4), 0x1000);`
- **UNVERIFIED:** that job threads write this dword concurrently.

### phase1.cpp 10081-12113

**W-P3-1. 11340-11381: the peer counts as absent on every frame without a packet**

```cpp
int nb = Net::DrainRemote(batch, 8);
bool gotRaw = (nb > 0);
...
bool rawSameWorld = gotRaw && localOk && raw.worldId == local.worldId && protoOk && sameScene;
...
bool peerPresent = rawSameWorld && (now - lastFreshMs) < kPeerLiveMs && timerOk;
```

- **Why:** `DrainRemote` pops a queue and returns 0 when it is empty (net.cpp:1888-1899). The replaced
  `GetRemote` kept returning the last packet. The comment at 11336-11337 ("all the logic below is
  unchanged") is false.
- **Failure:** `peerPresent` is false on packetless frames. The drive at 11530
  (`localOk && peerPresent && ...`) and the remote state at 11560 run only on packet frames, which
  defeats the per-frame interpolation. `[E] link-up/link-down` (11383-11397) flaps on every gap.
- **Fix:** keep the last valid packet in a static, update it on drain, and derive
  `rawSameWorld`/`protoOk` from it.

**W-P3-2. 11486 / 11511: liveness by vtable alone; the world-change path can `RemoveEntity` an innocent entity (UNVERIFIED heap reuse)**

```cpp
__try { puppetAlive = readable(g_puppet, 8) && (*(void**)g_puppet == g_puppetVtbl); }
```

- **Why:** the comment at 11480-11481 assumes reused memory gets a different vtable. All characters
  share vtable 0x01E4CE90 (11757, 11780).
- **Failure:** a new character at the old address passes the check. The world-change gate then calls
  `DespawnPuppet()`, which removes that entity.
- **Fix:** check liveness by object id, as the ghost ship does (11631). On a world change, drop the
  reference instead of calling `RemoveEntity`.

**W-P3-3. 11629 / 11666 / 11701: a ghost ship with `g_ghostObjId == 0` is never dropped (UNVERIFIED)**

```cpp
if (g_ghostShip && g_ghostObjId) {
```

- **Failure:** no replacement is ever spawned, and after a world change it is driven again with
  `SetGlobalMatrixInternal` (11701), possibly into reused memory.
- **Fix:** when the id is 0, drop the ghost on any world-id change.

**W-P3-4. 11881-11883: `GetUnusedPadID` replacement returns pads the engine never offered (UNVERIFIED)**

```cpp
for (int i = 0; i < 8; ++i) {
    if (((void**)padMgr)[i] == nullptr) {
```

- **Why:** the original (decomp/candidates/00744CF0.cpp) walks the list returned by `sub_A0A790`. The
  override applies for the puppet's whole lifetime, including when the real player re-enrols.
- **Fix:** mirror the original walk, and scope the override to the puppet's own enrolment call.

### net.cpp

**N1. 566-569 + 588-589 (with 299-327): a lost epoch-start packet rebuilds records from a zeroed shadow**

```cpp
if (h.epoch != g_rxEpoch) {
    std::memset(g_rxShadow, 0, sizeof(g_rxShadow));
    g_rxEpoch = h.epoch;
}
...
ShadowSlot* sh = ShadowFind(g_rxShadow, ShadowId(kind, key, e.objId), true);
if (sh) e = sh->st, e.key = key, ...
```

- **Why:**
  1. The sender clears its shadow every 65 packets (270-274). The first packet of the new epoch
     carries full records, and the sender's shadow now holds them.
  2. Only pos, quat and action have resend counters (325-327). `hp`, `arch`, `stage` and `anim` are
     sent once.
  3. If that first packet, or one chunk of it, is lost, the receiver's first new-epoch packet is a
     delta. It zeroes the shadow (567) and decodes on top of zeros.
  4. Every record present (any moving guard, which gets a pos resend) then decodes with `hp = 0`,
     `arch = 0`, `stage = 0` and `anim.valid = 0`, and those zeros are written back as the new base (627).
- The same happens to a body whose first-ever record is lost. A plain lost delta leaves *stale*
  values; this case leaves *zero* values.
- **Failure:** `arch = 0` switches off the archetype check in binding: `if (es.arch && ...)` at
  phase1.cpp:6925, `!es.arch || ...` at 6941 and 6887. A guard can bind to a body of another class,
  and sticky bindings outlive the epoch (~3.2 s at SnapshotHz=20). `stage` and `anim` also read as
  "none" until the next epoch.
- **Fix:** on the sender, force the full mask for the first `RESEND_AFTER_CHANGE` snapshots of a new
  epoch and of a newly created shadow slot. Or, on the receiver, keep the old shadow on an epoch change
  until a full record for that id arrives.

**N2. 658-661 (+ 181-182, 884-889): stale marker survives a host restart; entity snapshots blacked out for up to 4096 snapshots**

```cpp
if (g_rxHaveDone) {
    const int32_t d = SeqDiff(h.seq, g_rxDoneSeq);
    if (d <= 0 && d > -STALE_WINDOW) return fail(DEC_STALE);
}
```

- **Why:** a host starts `g_entSeq` at 0 (phase1.cpp:2813/3388). `g_rxHaveDone`/`g_rxDoneSeq` are reset
  only by the self-test, never on peer change: not on the host re-lock at 884-889, and not on the guest
  side.
- **Failure:**
  1. The host process restarts while the guest keeps running. In lobby mode the port is the same, so
     the guest's peer still matches.
  2. If the old host's last seq was below 4096 (about 205 s at 20 Hz), every new snapshot has d in
     (-4096, 0] and is dropped as `DEC_STALE`.
  3. The blackout lasts until the new seq overtakes the old one, up to about 205 s. The comment at
     654-656 claims a restarted host "passes".
- **Fix:** reset `g_rxHaveDone`, `g_asm` and `g_rxShadow` on a peer (re)lock. Or put a per-process
  session nonce in the header and reset when it changes.

**N3. 1149-1154: TOCTOU in `RelayJoinThread`; a WELCOME just before the unconditional `PeerSet` kills the link**

```cpp
if (!alive()) return 0;   // WELCOMEd directly (or superseded)
dbg("[Lobby] join: no direct answer in 3 s - using the relay\n");
...
PeerSet(g_relaySrv, Relay::kHostSlot);
```

- **Why:** a direct WELCOME can be handled by the recv thread between the `alive()` check and
  `PeerSet` (the window includes `dbg()`, a file write plus `fflush`).
  - `HandleLobby` sets `g_peer = hostPub` and `joinState = 2`.
  - This thread then overwrites `g_peer` with the relay server.
- **Failure:**
  - The host has locked the guest's direct address. The guest now sends via the relay, so the host's
    strict-peer gate (1445) drops relay-sourced datagrams.
  - The host sends directly, and the guest drops them (source is not the relay).
  - `joinState == 2`, so `JoinByCode` refuses a retry ("already connected"). The link is dead until
    restart.
  - The same TOCTOU lets an old relay-join thread overwrite the peer of a newer F8 attempt.
- **Fix:** do the `PeerSet` only via a compare on `g_joinState == 1` under `g_peerLock`, or set the
  relay peer before the last `alive()` check and re-check afterwards.

**N4. 867: the host ignores the same nonce on a second path forever, even when the locked path never carried a packet (UNVERIFIED frequency)**

```cpp
if (ready && p.nonce == g_lockedNonce) return;
```

- **Why:** this runs before the 10 s silence rule (873-875). The comment at 863-866 targets HELLOs
  arriving on two paths at once.
- **Failure:** a relay guest's direct HELLO reached the host, but the host's WELCOME did not reach the
  guest (inbound-filtering firewall). The guest falls back to the relay with the same nonce. Each relay
  HELLO is silently dropped, and the join fails after 10 min.
- **Fix:** allow a re-lock for the same nonce when `g_lastRecvMs == 0` and more than ~1 s has passed
  since the lock.

### deref_guard.cpp

**D1. 49-52 / 84-85 / 435-443: high pointer bound `0x7F000000` rejects valid large-address-aware objects (UNVERIFIED)**

```cpp
const uint32_t  MAX_TARGET_PTR = 0x7F000000;
```

- **Why:** phase1.cpp:233-234 records that valid pointers above 2 GB exist in this process ("wrongly
  rejecting valid large-address-aware pointers >2GB"). The stubs send any `[node+0x114/0x118]` at or
  above 0x7F000000 to the skip target: the action-lane update or the virtual call is skipped, or
  0x6795FD takes the "no target" path. The vtable-in-image test (2A-34) already rejects null,
  0xFFFFFFFF and wild pointers.
- **Failure:** silent missing engine updates for legitimate objects that are heap-allocated high, on
  all seven guarded sites (default ON: `GuardNullTarget=1`, `GuardStaleTarget=1`).
- **Fix:** drop the high bound, or move it to 0xFFFF0000 (sentinel only), and rely on the vtable check.
- **UNVERIFIED:** the exe's `IMAGE_FILE_LARGE_ADDRESS_AWARE` bit (not readable here), and whether
  these node targets are ever allocated above 0x7F000000. The only logged >2 GB `ent=` value in the
  repo (`0xCA0BB9FE`, RUN-2026-10-29-frozen) is garbage from C1, not evidence.

---

## C. Minor

### phase1.cpp 6001-8040

- **M-P1-1. 6105:** `static int64_t OwnOffset() { return s_offCur < s_offPrev ? s_offCur : s_offPrev; }`
  - It reads two int64 values with no lock. The callers run after the shared lock is released
    (6596, 6687), while the writer holds the exclusive lock (6100-6104).
  - On x86 the read tears, so one frame gets a wrong render time.
  - Fix: copy the offset under the shared lock.
- **M-P1-2. 7505-7509:** `if ((int)pkt.count > s_selfMax) pkt.count = (uint32_t)s_selfMax;`
  - A negative `EntitySelfTestMax` (with `EntitySelfTest=1`) gives count 0xFFFFFFFF, and the loop at
    7509 writes past the static `pkt`. The fault is caught by SEH, but neighbouring statics are corrupted.
  - Fix: clamp to at least 0.
- **M-P1-3. 7133:** `if (slot < 0 && s_lastN < MIRROR_TABLE) { ... }`
  - The table is never evicted. Once 512 bodies are seen, every new body takes `dp = 1e30`, so
    `bigJump` is always true and physics is always pushed, the jitter 7125-7128 says was removed.
  - UNVERIFIED that 512 distinct addresses occur live.
  - Fix: evict entries, or treat "no slot" as `dp = 0`.
- **M-P1-4. 6863-6894 / 6927-6931 / 6962:** the one-body-per-record `usedEnt` check runs only on the
  position scan.
  - The sticky and resolver paths skip it, and `objId == 0` has no steal check, so two host records
    can drive one body.
  - Fix: check `usedEnt` after `best` is chosen.
- **M-P1-5. 8011-8017:** `for (; q + 1 < qe && n < cap; ++q) { ... *(unsigned int*)(o + 0x60) ...`
  - It reads +0x5C..+0x63 past a bound that only covers +0x07. A match at a region end faults and
    aborts the whole scan. This code is probe-only.
  - Fix: `(char*)q + 0x64 <= (char*)qe`.

### phase1.cpp 8041-10080

- **M-P2-1. 8923:** `if (s_pushedDeathN < 256) s_pushedDeathIds[s_pushedDeathN++] = objId;`
  - The tombstone saturates at 256 deaths per scene, after which duplicate death pushes return.
    5046-5049 records the same shape already fixed elsewhere with eviction.
  - Fix: use a ring.
- **M-P2-2. 9160-9161 (UNVERIFIED):** `if (c.bhv && readable(c.bhv, 0x40)) { ((Component*)c.bhv)->SetEnabled(false);`
  - A raw component pointer is re-validated by `readable()` alone. If the guard is de-registered after
    SetLife(-1) (9062) and the memory is reused within 8 ticks, the interlocked write lands in an
    unrelated object.
  - 9139 `if (slot >= 64) slot = 0;` overwrites a live latch, which then never fires (same effect as W-P2-1).
  - Fix: re-check the component vtable (0x026E34D8) and that it is still in the entity's component list.
- **M-P2-3. 9308 / 9334 / 9359:** `CachedHealthForEntity`, `GetEntityAI` and `FindHealthInAI` run
  outside `__try` in `ApplyPeerHealth`.
  - `FindHealthInAI` reads `arr + k*4` for k up to 63 after `readable(arr, 4)` only (8240-8243).
  - Fix: wrap the resolve block.
- **M-P2-4. 10062-10068:** after a peer restart with an old seq of 1000 or less, every new player
  packet is dropped as a reorder until the new seq overtakes the old one, so the puppet freezes.
  - Fix: wipe on a persistent gap, or on a `hostTimeMs` or epoch jump.
- **M-P2-5. 9465:** the comment says the die-clip watcher is "always on", but `TickDieClipWatch` and
  `PollTrackedGuards` are commented out at 11263-11268.
- **M-P2-6 (policy, UNVERIFIED):** `GuestKillMirror` defaults to 1 (9108, 9112, 10916) and performs
  engine writes. AGENTS.md wants new behaviour at 0 until it is verified live; no live receipt was
  checked here.
- **M-P2-7. Dead code with latent bugs**, unreachable today:
  - `GpTrap` (8465) returns CONTINUE_EXECUTION for *every* guard-page violation while armed, so other
    components' guards are swallowed. It also never re-arms, which contradicts the 600-count logic at
    8457-8463.
  - `KillNearestGuard` reads 8207-8222 outside SEH.
  - `ScanShipHealth` (9620-9626) has the same +0x60 overread as M-P1-5.
  - Also unreachable: `ProbeReverseWalk`, `ProbeDeathDiff`, `ArmLifeWriteTrap`, `ArmGuardPage`,
    `ShipHealthDiff`, `ProbeHealthHeap` and `ProbeInputState`.

### phase1.cpp 10081-12113

- **M-P3-1.** Thread comments contradict the measured note at 5840-5843 (Human updates on worker
  threads): 12099, 11730-11731, 10190 and dllmain.cpp:563 say "main thread".
- **M-P3-2. 11801-11803:** calls SetGlobalMatrixInternal's physics gating "verified", but
  `decomp/units.json` 00528200 has `UNVERIFIED-EVIDENCE`.
- **M-P3-3. 10326:** cites "phase1.cpp:6088/:6066"; those lines are now 7888/7866. The cites to
  1009/1110/1228 at 10427-10429 and 10494 were not checked.
- **M-P3-4. 10906-10941:** `DumpConfigOnce` claims to list every key but omits:
  - Coop.AutoContinue, AutoContinueSlot, MenuProbe, SkipIntroVideos, UiProbe and VideoLog
  - Net.RelayForce, Upnp, Mode and JoinCode

  The defaults it does print match their call sites.
- **M-P3-5. 10436:** `const uintptr_t TP_PTR_HI = 0x7F000000u;` contradicts phase1.cpp:233-234, so
  TransformProbe can report `valid=0` for a valid >2 GB pointer. This is the same issue as D1, in a
  probe.
- **M-P3-6. Unreachable code:** `RequestShipSpawn`, `RequestShipDriveProbe` and `RequestShipProbe`
  have no callers.
  - `TryShipSpawn`, labelled observation-only, writes `*(int*)((char*)req + 0xC) = 0x13;` (10209)
    after the request has returned.
  - `g_holdShipEnt` is never reset on a world change (11278-11283).
- **M-P3-7. 10128:** `out = g_snaps[newestIdx].st; return true; // shouldn't reach` is reachable when
  `PuppetHostTime=1` timestamps step backwards (10052-10055), so the puppet snaps to the newest sample.

### net.cpp

- **N5. 887 (and 873):** `g_lastRecvMs = 0;` is a 64-bit write by the recv thread outside `g_lock`,
  while `LastRecvMs`/`MillisSinceRecv` (2003-2018) read it under `g_lock`.
  - Taking the lock on the reader side only does not prevent a torn read on x86, so one call can
    return a wrong age.
  - Fix: take `g_lock` around the write at 887, or make it an interlocked 64-bit store.
- **N6. 1998-2000:** compare, then reset `g_worldConfirmed`, then store the new id. If the recv thread
  decodes an old-world packet between the reset and the store, it sets `confirmed = 1` for the *new*
  world. The new world's `worldId == 0` bootstrap snapshots are then `DEC_WORLD`, the opposite of the
  intent stated at 1994-1997.
  - Fix: `InterlockedExchange64` the new id first, then reset the flag if the old value differed.
- **N7. 1501-1531 + 331:** non-resent fields are lost when two snapshots land in one frame gap.
  - `g_remoteEntities` is last-writer-wins, and unchanged records are omitted. An `hp`/`arch`/`stage`
    change carried in snapshot N is never seen by the frame if N+1 omits that record (nothing changed
    and no pos/quat/action resend pending).
  - The effect is bounded by the next epoch (~3.2 s). It matters for `DamageRelay=1` (default 0).
    (Omitted *fields* are safe: they are rebuilt from the shadow at 589.)
- **N8. Comment drift (no code defect):**
  - 193-194 and 226-227 give `ENTITY_CHUNK_DATA` as 1368 B and the count as "(16384-32)". The header
    is 36 B (netstate.h:296), so the payload is 1364 B. The `static_assert`s at 228-235 compute from
    the constants and hold (ceil(16348/1364) = 12; 11*1364 = 15004 < 16348).
  - 488 says `sizeof(EntityPacket)` is "~93 KB"; it is 20 + 128*106 = 13,588 B.
  - phase1.cpp:5536 says omitted fields arrive as 0. They are rebuilt from the shadow (net.cpp:589);
    the code there is conservative, so this is harmless.

### deref_guard.cpp

- **D2. 445:** `// 28  8B 01  mov eax,[ecx]  -- the vtable pointer; ecx is a readable object by now`
  - That is false. The range test does not prove the page is mapped, so a freed and decommitted target
    faults *inside the cave* with no SEH.
  - This is not a regression (the original `mov eax,[ecx]` faults too), but crash triage will see a
    cave EIP rather than an engine EIP.
  - Fix: correct the comment, and note the cave range in `[DG]` logs for triage.
- **D3. 893-900 + 954-1004:** with `FixNullReturn=1` and `NullReturnObserver=1` (both defaults), the
  0x6817FA call to `sub_733FB0` is re-issued from the Part C cave. `NullObsDetour` therefore records
  RA = cave+5, not 0x006817FF.
  - The observer's caller table names a heap address for that site.
  - Fix: map cave return addresses back to the site in `LogNullReturn`.
- **D4. 996-999:** `g_noCallersUsed` is incremented before `ra` is written.
  - A concurrent thread scanning `i < used` sees `ra == 0` for that row, fails to match, and claims a
    second row for the same caller, so counts are split.
  - The effect is diagnostic only.
  - Fix: write `ra` with an interlocked compare-exchange on the slot.
- **D5. deref_guard.h:26-27:** says the vtable must lie "inside .rdata (VA 0x01E3A000 + 0x0090874A)".
  The code accepts the whole module image [ImageBase, ImageBase+SizeOfImage) (deref_guard.cpp:70-75,
  1105-1113). The header is stale.

## D. UNVERIFIED risks (need disassembly to classify)

- **R1. deref_guard.cpp:41-63 / 405-482: the stubs clobber EFLAGS.** The stolen
  `mov ecx,[esi+OFF]` preserves flags, but the stub runs `test`, `cmp` and `sub` on both exits.
  - For Part B, the continue instruction is `test ecx,ecx`, which is byte-verified at install
    (`expCont`, 699/708), so that site is safe.
  - For the six Part A sites (continue VAs 0x6797DC, 0x6797EA, 0x681AED, 0x681B67, 0x681B29 and
    0x681B4F), it is unverified that the next instruction rewrites flags before any `jcc`.
  - Separately, the skip target 0x681B36 is documented as a "post-call cmp/je". If that `cmp` reads
    EAX (the skipped call's result), the skip path feeds it the caller's pre-load EAX (restored by the
    stub), which gives an arbitrary branch.
  - Needs: disassembly at each continue VA and at 0x681B36.
- **R2. deref_guard.cpp:756-769 / 893-900: the NULL exit skips the whole function body.** It leaves
  `sub_6817C0` at 0x6818E6 with EAX = 0, skipping 0x681805..0x6818E5.
  - If that span releases a lock, decrements a refcount or restores state taken before 0x6817FA, the
    exit leaves it unbalanced.
  - The epilogue `pop edi; pop ebx; pop esi; mov esp,ebp; pop ebp; retn` (byte-verified at 771/861) is
    stack-correct only if nothing is pushed and outstanding at 0x6817FA.
  - Needs: a bounded disassembly of 0x6817C0-0x6818EC.

## E. Checked, no defect found

- **net.cpp, encoder bounds:** the worst-case record is 16+12+16+4+2+2+1+1+21+32 = 107 B, and
  128 x 107 + 36 = 13,732 B, which is below 16,384. So the `break` at 348 is unreachable, and so is the
  early decrement of the resend counters at 325-327 for a record that does not fit.
- **net.cpp, header layout:** the self-test pokes chunk fields at 28/30 (511), which matches
  `EntityWireHeader` (netstate.h:282-296).
- **net.cpp, decoder:** every optional field is bounds-checked (590-612). `nBlend` is clamped to
  `GUARD_ANIM_BLEND_MAX` on decode (610) and on encode (346). The mask whitelist (580) and the kind
  whitelist (584) are in place.
- **net.cpp, chunking:** chunk index and total are validated (645-646). Reassembly bounds hold (709).
  Duplicate chunks are ignored (707). A newer seq discards an older set (686). A single-chunk snapshot
  makes later chunks of an older set `DEC_STALE` through the marker (658-661).
- **net.cpp, player lane:** exact-size gate (1441) plus a proto gate (1468), and `blendCount` is
  clamped (1502).
- **net.cpp, relay datagram size:** 1400 + 16 = 1416, within `Relay::kMaxDatagram` (1500).
- **net.cpp, kill records:** they share the objId shadow slot with the guard's snapshot record
  (`ShadowId` ignores kind when objId != 0). Tx and rx update that slot symmetrically. If a kill packet
  is lost, the next snapshot re-sends every differing field, and the decoder's `EF_DEAD`
  normalisation (621-623) restores `hp`.
- **net.cpp, unchanged bodies:** a perfectly static body is still re-sent every epoch (~3.2 s), so the
  10 s guest slot-stale rule (phase1.cpp:7203) does not release it.
- **deref_guard.cpp, site table:** offsets match every documented literal (6797D6, 6797E4, 681AE7,
  681B61, 681B23 and 681B49; skips 6797E4, 6797F5, 681AFA, 681B36, 681B5C and 681B75). No continue or
  skip target lands inside another patched 6-byte site, except site 0's skip, which deliberately enters
  site 1's stub.
- **deref_guard.cpp, stub balance:** stack balanced on all paths (2 pushes, 2 pops). PIC rel32 targets
  are computed after the cave address is known.
- **deref_guard.cpp, `NullObsDetour`:** the stack walk is correct (`[esp+4]` = savedRA after
  `push eax`), and EAX is preserved.
- **deref_guard.cpp, patch timing:** patching runs at `DLL_PROCESS_ATTACH` (dllmain.cpp:573), before
  WinMain, so engine threads cannot execute a half-written site.
- **phase1.cpp, `SgmDetour`:** PUSHAD offsets are correct (ECX at +0x18, RA at +0x20, args at
  +0x24/+0x28 for a jmp-entered detour).
- **phase1.cpp, flag defaults:** the defaults documented as 0 in these ranges really are 0:
  - `GuardInterpBuffer`, `StateProbe`, `EntityFrameCadence` and every `OwnFlag` key (5857)
  - `WatchTarget`, `WatchDeadBit`, `PuppetHostTime` and `BindRadiusM`
  - `GuardStaleReturn` (deref_guard.cpp:873)
- **deref_guard.cpp, flag defaults:** `GuardNullTarget`, `GuardStaleTarget`, `FixNullReturn` and
  `NullReturnObserver` default to 1, and are documented as 1 (deref_guard.h:31-33, dllmain.cpp:571).
