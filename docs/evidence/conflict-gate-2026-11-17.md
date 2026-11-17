# Conflict gate: why guards never start a fight with the puppet (2026-11-17)

This checks the overnight static claims (`ops/MASTER-LEDGER.md`, "conflict layer mapped") in IDA. It then pre-registers one
live read.
- AC4BFSP.exe md5 `2058342866688f780c8b34526a65bc35`.
- IDA batch jobs: `D:\ac4ida\cc\jobs\v1`, `v2` (decompile, disasm, callsites).

## 1. Checked in IDA

| claim (ledger) | verdict | receipt |
|---|---|---|
| `sub_1625330` (start conflict) has 16 call sites, including the enlist `sub_1625590` | **VERIFIED** | xrefs_to 0x01625330: 16 code calls; one, 0x015B5324, is in no defined function |
| it runs only when the attacker has no round | **VERIFIED** | `if (!*(a2+0xE8) \|\| !*(*(a2+0xE8)+0x98))` at entry; `a3` (target) must be non-null |
| it needs both sides' team non-null with `team+0x18 != 0` | **VERIFIED** | `v13 = sub_15E50F0(a2)`, `v14 = sub_15E50F0(a3)`; `if (v13 && v14 && *(v13+0x18) && *(v14+0x18))` encloses the whole conflict body |
| "silent no-op" otherwise | **CORRECTED** | Not a no-op. Before the team test it runs `sub_15E5E00`, may zero `fm+0x10` for a kind-1 target, runs `sub_1622990(a2,a3)` (creates teams and rounds, below) and `sub_15DB9F0(a3,a2)` |
| team lookup `sub_15E50F0` = ResolveFaction, then a scan of fm+0x30C for `+0x18 == faction` | **VERIFIED** | first match wins; array `fm+0x30C`, count u16 `fm+0x312` |
| ResolveFaction `sub_133A4C0` reads `ai+0xA4` | **VERIFIED, with a fallback** | `ai = sub_6F5F70(e)` = `*(*(e+0xE8)+0x1C)` (decompiled); if `sub_F916A0(ai)` it returns `*(ai+0xA4)`, **otherwise it returns a handle decoded through `sub_11DDD60(e)`**. So `fac` is the AI field only when the type gate passes (the probe logs `tg=`) |
| "equal keys merge (`sub_1621670`)" | **CORRECTED** | `sub_1621670(fm, team, a3)` **removes and destroys** a team: it unlinks it from fm+0x30C, deletes it (vtable slot 0, then the arena free) and re-dispatches its members. The merge is in its caller `sub_1622990`: when both teams were just created and carry the **same** key, the attacker's new team is removed and the function returns 0 (no conflict) |
| round creation `sub_1620140` -> `sub_161D810` -> `sub_161BE20` -> `sub_156D040` (alloc 0x3B0, ctor `sub_1561050`, append `sub_154DB60`) | **VERIFIED** | Disasm confirms the dropped thiscall arguments. `sub_161D810` is thiscall (fm, team, entity), with `retn 8` and a lock on fm+0x66C. `sub_161BE20` calls `sub_156D040` with ecx = team and entity pushed (0x0161BE2F-0x0161BE33) |
| creation needs `*(entity+0x5C)+0x8DC` non-null | **VERIFIED** | `sub_156D040`: `v9 = *(a2[23] + 0x8DC)`; `if (!v9) return 0` |
| team key = faction of its first member | **VERIFIED (new)** | `sub_156D040`: `if (!*(u16*)(team+0xE)) *(team+0x18) = sub_133A4C0(entity)` |
| teams are created on demand | **VERIFIED (new)** | `sub_1622990`: when an entity has no round and no team matches its faction, it allocates 0x148 bytes, runs ctor `sub_1561DC0(fm->+8 + 0x888, fm)`, then `sub_161D810(team, e)` |

### What this means for the puppet (static, both binaries not checked: SP only)
- A guard (attacker `a2`) engaging the puppet (`a3`) creates or reuses a team for the puppet, keyed by
  `ResolveFaction(puppet)`.
- **If that key is 0, the conflict body is skipped** (the `*(v14+0x18)` test). If the puppet's key equals the guard's,
  `sub_1622990` returns 0 when the teams are new, and the conflict body is skipped as well.
- In both cases no error is raised; the guard keeps its target and never fights.
- UNVERIFIED until read live: the puppet's actual key. The 11-16 probe pass logged `acP=0` from an older probe line
  (different field read); that run is not this read.

## 2. Pre-registered live read (written before the run)
- Build: branch `re/conflict-gate-probe` (from `feat/enemy-placement` 798b039) + `[PL] gate` line in `src/puppet_listener.cpp`.
- Flag: `[Coop] PuppetListener=1`, report only.
- The line logs, for the puppet and the local player side by side:
  - ResolveFaction;
  - the team it resolves to in fm+0x30C, with that team's key;
  - `ai+0x98`.
- Rig: two copies on this PC; the guest provokes soldiers next to the host. Read on the **host**, where the puppet is the
  guest's body.
- **Control:** the player's own `fac` and team, in the same line. Guards do fight the player, so the player's key must be
  non-zero; if it is 0, the read is broken.

| outcome | verdict |
|---|---|
| puppet `fac == 0` (or puppet team key 0) while the player's is non-zero | **PASS**: the faction key gate is what refuses the puppet |
| puppet `fac` == the guards' faction (puppet sits in the guards' own team) | **PASS (variant)**: same-team refusal in `sub_1622990` |
| puppet `fac` non-zero, different from the guards', team non-null with key != 0, and still no round (`pupRec98=0`) while guards engage | **FAIL**: the gate is not the block; look upstream (`sub_1622990` not reached) |
| player `fac == 0` | **INVALID**: the read is broken |

## 3. Result: **FAIL**. The faction and team gate is not what refuses the puppet
Two copies on this PC, started from the exe, read on the host. Each `[PL] gate` line is one poll per second.

**Pass 1** (asi `a10c6ce7…`, commit 798b039 + the gate line; host pid 20432):
- puppet `fac=FFADF4A0` with `tg=1` (the AI field is used, not the fallback). This is the **same** value as the player's.
- The team list stayed empty (`teams=0`) for all 137 polls, because the host's own player never fought, so the run had no
  positive control.
- It also exposed a probe defect. `rec98` reads `ai(+0x1C)+0x98`, but the engine reads the round at
  `AIComponent(+0xE8)+0x98`. The player's value at the wrong field was `3C82C103`, which is unaligned, so it is not a
  round pointer.
  - The older `rec98` / `playerRec98` columns, and the ledger's "AI+0x98" wording, all describe that wrong field.

**Pass 2** (asi `23298d3c…`, round read fixed to `*(e+0xE8)+0x98` plus its `+0x10`; host pid 26688; host player fights
30 s first as the control, then the guest provokes soldiers for 75 + 42 beats):

| phase (polls) | teams | puppet: fac / team / key / round | player: fac / team / key / round -> round+0x10 |
|---|---|---|---|
| before any fight (55) | 0 | FFADF4C0 / 0 / 0 / **0** | FFADF4C0 / 0 / 0 / 0 |
| **host player fights (40)** | **2** | FFADF4C0 / **3E327F60** / **FFADF4C0** / **0** | FFADF4C0 / 3E327F60 / FFADF4C0 / **429B7CB0 -> 3E327F60** |
| guest provokes soldiers (81) | **0** | FFADF4C0 / 0 / 0 / **0** | FFADF4C0 / 0 / 0 / 0 |

- **Control PASS.** In a real fight the player gets a round. The round's `+0x10` is its team, and that team's `+0x18` is the
  player's faction.
  - This confirms live the static chain: `sub_156D040` sets the key; the `sub_15E50F0` lookup; the round at
    `AIComponent+0x98`.
- The puppet's faction is non-zero and resolves to the **player's own team**, whose key is non-zero.
  - So the `*(v14+0x18)` test in `sub_1625330` would pass for it.
  - So would the same-key test in `sub_1622990`: a guard's faction differs from the player's, since that team fought the
    guards.
- While the guest fought, **no team existed on the host at all**. `sub_1622990` creates a team for each side that has no
  round. When both sides' teams are new and their keys are equal it deletes only the attacker's, so the target's survives;
  otherwise it keeps both.
  - So one run of it with the puppet as the target would have left at least one team.
  - Zero teams over 81 one-second polls means **the conflict starter was never reached for the puppet**.
  - UNVERIFIED: a team created and torn down within one poll interval. The player's teams persisted for the whole 30 s
    fight.
- Note: the faction address differs between the passes (FFADF4A0 vs FFADF4C0). It is a per-session pointer-like value, the
  same for both bodies within each run.

### Consequence
- The block is **upstream** of the conflict layer: nothing in the guard's code asks the FightManager to start a fight with the
  puppet.
- Next read: which of the 16 callers of `sub_1625330` (or the attack API `sub_1620140`) fires for the player. Hook the entry of
  `sub_1622990` / `sub_1625330` with `a2`/`a3` and the return address, and compare player and puppet in one run.
- Do not spend more static effort on the faction or team key; it is live-settled.

## 4. Trace: who starts a fight for the player, and why not for the puppet
- `[Coop] ConflictProbe` (default 0, `src/conflict_probe.cpp`, read-only entry detours) hooks four functions, all verified in
  IDA jobs v3-v12:
  - `sub_1625330` StartConflict: thiscall, `retn 0Ch`;
  - `sub_1620140` attack API: thiscall, `retn 0Ch`;
  - `sub_1342F70` BuildFightBehaviour: cdecl;
  - `sub_15B37E0` ReactionDrive: thiscall.
- Each run is the same rig as pass 2: the host player fights for 30 s, then the guest provokes soldiers.

**Run 3** (asi `af7d2a60…`, host 27616; first two hooks only):
- 6 StartConflict calls in the whole run; the attack API was never called.
- All 6 were a guard against the **player**, from one path: `0x01343008` (in `sub_1342F70`) ← `0x01343D99` (in
  `sub_1343D30`) ← `0x01587254` (in `sub_1587230`).
- None involved the puppet.

**Run 4** (asi `87004874…`, host 5920; adds the `sub_1342F70` hook):
- `sub_1342F70` was entered only 6 times, all guard → player with `a4=1`, all from `0x01343D99`.
- **It was never entered with the puppet**, so its internal refusals are not what blocks it.
- Those refusals are: null entity or world; `sub_133FAA0(target)`, which is target AI `vt+0x34` = `sub_753330`:
  `(*(ai+0x98) && **(ai+0x98)) || *(u8*)(ai+0x8C)`; and same or allied faction (`sub_1711500`).
- The player's AI has the same vtable `0x026D3CD8` and that slot; its refusal read 0 (`b8C=0`, `b98=0`).

**Static chain** (decompiled):
- `sub_15B37E0` drives a reaction at `this+0x88` whose target is the entity in the holder at `this+0x80` (`+8`).
- Gate: `react->vt[0x20](react, *(this+0x70), holder)`.
- Then `sub_1587230` calls `react->vt[0x24]`. For the fight-start reaction, vtable **`0x026B1BF0`** (ctor `sub_1324330`,
  19 slots), slot 9 is `sub_1343D30`, which calls `sub_1342F70(…, a4=1)` and then `sub_1625330`.
- `sub_1343D30` is reached only through that vtable (data ref 0x026B1C14).
- CRLDetect's start `0x0131EF30`, which writes its target from `sub_70E810` = the world's player, belongs to a
  **different** vtable (0x026B1D80 area). It is not on this path.

**Run 5** (asi `05b32a59…`, host 36044; adds the `sub_15B37E0` hook, logs each new (reaction vtable, target) pair):

| target | reaction classes driven (vtable) |
|---|---|
| PLAYER | 026A0A10, 02719BB0, **026B1BF0 (fight start)**, 027053F8, 026FE7D0, 02712E08 |
| npc | 026DC3C0, 026E36A0, 027053F8, 026FE7D0, 02712E08, 026ED9A0, 02696500, 02721820, 026D81E0 |
| **PUPPET** | **026A0A10 only**, once, during the guest's fight phase (t=257307734) |

- Counts at the last conflict start: drive=103, of which 51 targeted the player and 0 the puppet. The one puppet reaction
  came later.
- **Verdict:** guards do react to the puppet; the reaction driver ran once with it as the target. But they **never
  choose the fight-start reaction (`0x026B1BF0`)** for it, and for the player they do.
- The divergence is in **reaction selection**, upstream of `sub_15B37E0`: the code that creates a reaction object and fills
  `this+0x80` / `+0x88`.
- UNVERIFIED: what class `0x026A0A10` is. Its name is not resolved; the next step is its class-info hash, via `known.py`.

## 5. Reaction selection: the puppet emits almost no stimuli
Reference used: AU-RW `origin/acu-85-coop-re-plan`, ACU-RE-0091 / 0092 / 0102 / 0105.
- ACU's newest fighting RE covers damage routing to the owner, health authority and the lockstep fight input. Awareness
  and reaction selection are still "Everything open" there (`docs/coop/COVERAGE.md`).
- The one transferable point: ACU NPCs fight a remote player because its copy is a real **player-kind body**. Kind bits
  are `(entity+212) & 7`, with 1 = player avatar, plus replica role bits (ACU-RE-0092 obs 10-12, 17).
- In AC4 the same kind test exists at `entity+0x9C` (`sub_1625330`, `sub_15AE100`, `sub_15A1650`).

**Static** (jobs v13-v18):
- The selector is `sub_15AE100(mgr, stim, float)`, thiscall `retn 8`. Its callers are `sub_15B36C0`, `sub_15B6AD0` and
  0x015BD656.
- Stimulus layout: source holder at `stim+8`, id at `stim+0x10`.
- It returns early when `sub_15A1650` filters the stimulus out (by the reacting body's kind word `>>3 & 0x3F`, the id, and
  distance against the per-id radius table `*(*(0x4DE6D4C)+500)[id]`), or when the id has no entry (`sub_10D7F10`).
- Reaction set: `sub_1A61820(…)`, which picks by the flag bits at `stim+0x14` (1/2/4/8/16), or the default set
  `*(sub_10A9F80()+92)`.
- It installs `mgr+0x80 = stim->vt[0x20]()`, a clone of the stimulus, so **the reaction's target is the stimulus source**,
  and `mgr+0x88 = reaction`. The reacting body is `mgr+0x70`.
- The fight-start reaction's ctor `sub_1324330` has no direct callers (factory). The other `+0x88` writers in the band are
  `sub_15ADB70`, `sub_15B2130` (a serializer) and the driver itself.

**Live:**
- **Run 6** (asi `5c0118d9…`, host 22284; adds the `sub_15AE100` hook):
  - Stimuli reaching guards' selector: **player 214**, ids 22, 391, 158, 257, 135, 8, 258, 153, event vt `026E47C4`.
  - **Puppet 1**: id 105 only, event vt `026E4798`.
  - Both bodies report kind word `0x00000001` (`&7 = 1`, player avatar), so **kind is not the difference**.
- **Run 7** (asi `cb92cd48…`, host 34056; the reaction driver also logs the stimulus id inside the driven reaction):

| stimulus id (source PLAYER unless noted) | reaction class driven |
|---|---|
| 391 | 026A0A10 |
| 158 | 02719BB0 |
| **257** | **026B1BF0 (fight start)** |
| **135** | 02719BB0, 027053F8, 026FE7D0, **026B1BF0**, 02712E08 |
| 258, 153 | 02712E08 |
| **8, source PUPPET** (once) | 02719BB0, **026B1BF0** |

- In run 7 the puppet emitted stimulus **8 once**, at t=260176453. **The whole chain then ran for it**, in the same
  millisecond window:
  - select;
  - fight-start reaction against the PUPPET;
  - `sub_1342F70` build: target AI vt `026D3CD8`, refuse=0;
  - `StartConflict(guard 2EF71240, PUPPET)` from 0x01343008.
- On the next `[PL] gate` poll **the puppet had a round** (`3C5168A0` → team `3F1608A0`, key = its faction). One poll later
  the team list was empty again.
- **Verdict:**
  - **Block 1:** guards start a fight only on a fight-trigger stimulus (seen: 257, 135, 8) whose source is the target. The
    replay-driven puppet almost never emits one: 1 stimulus per run against the player's ~200.
  - When it does, the engine starts the conflict with the puppet normally: team, round, key.
  - **Block 2:** that conflict is torn down within about 1-2 s.
- This agrees with 2026-11-13 (`player-stimuli-2026-11-13.md`):
  - PuppetStim mode 2 posting 48/49 from the puppet put guards on alert;
  - the fight-member loop then crashed in the puppet's nav service;
  - with `FightSkipPuppet`, guards stood without moving or attacking.

## 6. Injected fight-trigger stimuli: guards fight the puppet natively
**Run 8** (asi `cb92cd48…`, same build as run 7; host 35300, guest 20780; scratch rig `cg_run2.ps1`):
- Flags: `PuppetStimMode=2`, `PuppetStimIds=8,257`, `FightSkipPuppet=0` (the key is present, so `NavNullGuard` installs),
  `NavNullGuard=1`, `FightProbe=1`, plus the probes.
- The host player fought for 30 s, then the guest fought for 60 s while the host fired PuppetStim from the puppet 5 times,
  8 s apart.
- Each fire posted id 8 (radius 30 m) to 30-66 NPCs and id 257 (radius 25 m) to 15-51.

**Results:**
- **The host player died** in the control fight (life 0). A scene change followed, and the puppet was re-spawned as a new
  entity (3DFE5670 → 31ADF4A0).
  - The only `[FightRm]` for the puppet happened in that reload: `from=01621A3C` (inside `sub_1621670`, RemoveTeam) ←
    0x0161F7CE ← 0x01620139.
  - **So this run gives no evidence of a conflict-end teardown;** the run 7 collapse is still unexplained.
- **After the re-spawn**, guards against the puppet, while the host player was not fighting (`plRound=0`):
  - 7 `StartConflict(guard, PUPPET)` over the run, all from 0x01343008;
  - reactions `026B1BF0` (fight start) on stimulus 257 and `02712E08` on 258;
  - the puppet **held a round for 52 one-second polls**, with 2 teams, through the end of the run;
  - `[PoiFO] targetIsPup=1` and fight groups of 3-6;
  - **13 guard hits on the puppet** (`[PDP] hit-post puppet`, damage 20 each, attacker `2EFBA260`), each forwarded by
    `PlayerDamageRelay` (`[PDR] tx hit #2…#14`).
  - The guest logged 14 `owner apply hit damage=20`. 13 of those name an attacker with **no linked copy** on the guest
    (`local 00000000`). UNVERIFIED whether the guest's life changed: the line does not log it.
- **Control:** runs 2-7 used the same guest-fight phase without injected stimuli, and gave 0 conflicts on the puppet (run 7:
  one natural id-8 stimulus, then a collapse).

**Verdict:**
- The F7 measured fact ("guards never move or attack the puppet") is **block 1 in this configuration**: the puppet does not
  emit the player's fight-trigger stimuli.
- With them injected, the engine's own reaction → fight-start → conflict → attack path runs against the puppet, and guards
  hit it repeatedly.
- **Not established:**
  - whether guards *move* to it (no position check was logged);
  - whether `FightSkipPuppet=0` with `NavNullGuard=1` is required. 11-13 crashed without the guard;
  - why run 7's single conflict collapsed;
  - whether PuppetStim mode 2's direct post (it bypasses the engine's broadcaster and its receiver filter
    `sub_133FA50(e+200)`) is equivalent to a native emit.
- Oracle level: log counts (3), no visual check.

## 7. `[Coop] PuppetFightStim` (default 0): the puppet emits the fight trigger natively
**Emit sites of the player's fight-trigger stimuli** (EventProbe across 6 runs, then IDA job v19/v20). Each is posted by
**guard-side** code about the player, not by the player's body:

| id | poster (return address) | what it is |
|---|---|---|
| 8 | `sub_102DD90` (0x0102DE47), 4 calls from `sub_10320C0` | the guard's sight sensor posts "seen X" to its own guard (`*(this+0x78)`): `+0x10` = id, `+8` = X's handle, `+0xC` = guard's handle |
| 257 | `sub_17B7A80` (0x017B7B1C), from `sub_17BE5C0` x3, `sub_17BEC90`, 0x017BEAAF | conflict statechart: posts 257 to `*(this+0x5C)`, `+8` = the handle at `this+0x6C`, `+0xC` = receiver's handle |
| 135 | `sub_10608A0` (0x010609E3, from `sub_1084920`) and `sub_1654F40` (0x016550E5, from `sub_1657FD0`) | relays: forward an existing stimulus to bodies within its id's radius |

- Design: the host already knows which guard the puppet fights. `FightIntentApply` (phase1.cpp, `[Coop] FightIntents`) applies
  the guest's fight enter / attack to the puppet with that guard resolved.
- At that moment the puppet posts `PuppetFightStimIds` (default `257`) to that guard, laid out exactly as `sub_17B7A80`:
  - `sub_FA3260` builds the stimulus;
  - `+0x10` = id, `+0xC` = guard handle, `+8` = puppet handle;
  - `sub_64A020(ev, guard)` posts it.
- While the puppet stays in fight (`path[2]==163`) it reposts to its last target every `PuppetFightStimMs` (default 1000).
- Correction to the old `PuppetStim` mode 2: it writes the **puppet's** handle into `+0xC`, where the engine puts the
  receiver's. That is left as it was (a test tool); the new emit follows the engine.

**Pre-registered A/B (written before the run):**
- Rig `pfs_ab.ps1`: two copies; the host player idle; the guest fights the soldiers for 75 s; the same build for both arms;
  only `PuppetFightStim` differs.
- Both arms: `FightIntents=1`, `NavNullGuard=1`, `FightSkipPuppet=0`.
- **PASS:**
  - arm 1 has ≥1 `StartConflict(guard, PUPPET)`, a puppet round present for ≥5 polls, and ≥3 guard hits on the puppet;
  - arm 0 has ≤1 conflict on the puppet;
  - **movement:** in arm 1 the hitting guards' `dist` at a hit is smaller than the same guard's `dist` at its conflict start,
    or the start is already within melee range (≤ 2.5 m), which is then reported as such.
- **FAIL:** arm 1 is no better than arm 0. **INVALID:** no `[FI] apply enter` on the host in arm 1 (the trigger never ran).

**A/B results:**

**Round 1** (asi `45d1c87a…`): **INVALID.**
- Arm 1 logged `PuppetFightStim=0`. The flag is read at the first fight intent, after `two_copy.ps1` has restored the ini.
- The rig now writes the arm's keys again after the copies start.

**Round 2** (same asi; host idle; 2 reps per arm): **INVALID for the flag, informative otherwise.**

| arm | conflicts on puppet | round polls | guard hits |
|---|---|---|---|
| 0 (rep 1) | 3 | 23 | 0 (one copy crashed at 0x0188F161, the sight-check family; `SenseOwnerGuard` is not on this branch) |
| 1 (rep 1) | 8 | 82 | 10, but **all 14 posts faulted** (`posted=0`) |
| 0 (rep 2) | 5 | 82 | 3 |
| 1 (rep 2) | 4 | 72 | 2, posts "succeeded" with a wrong source |

- **Defect (mine):** `FightIntentApply` passes the puppet's **Human component** (`s_pupHumanKey`), not its entity, and
  `+0xC8` was read off it. Fixed:
  - the source is now `Phase1_PuppetEntity()`;
  - `PostFightStim` refuses unless each handle decodes back to its own entity (`(h[2]>>31) & h[0]`).
- **Correction to §5's verdict:** with the host player idle, guards received **257 and 135 about the puppet naturally**, and
  ran the fight-start reaction in every rep of both arms.
  - The near-zero puppet stimulus count of runs 2-7 was measured while the host player fought first.
  - So the deficit depends on the situation (the host player's own fight). It is not absolute.
- **Movement, both arms:** conflict starts at 4.9-47 m; every hit at 0.7-1.6 m. **Guards close on the puppet.**

**Round 3** (asi `57f36c52…`, the fix; the host player fights 30 s first, then the guest fights 75 s; 1 rep per arm):

| arm | PFS posts (ok / tried) | conflicts on puppet | round polls | guard hits on puppet (dist) | player conflicts |
|---|---|---|---|---|---|
| 0 | 0 / 0 | **0** | **0** | **0** | 5 |
| 1 | 2 / 2, 0 handle failures, 0 exceptions | **3** | **8** | **4** (1.41-1.50 m) | 5 |

- Arm 1 chain:
  - both posts went to guard `30131BE0` on the guest's attack intents;
  - that guard ran reaction `026B1BF0` on stimulus 257 and started a conflict on the puppet from 2.64 m;
  - it then hit twice at about 1.5 m;
  - a second guard (`3CD803E0`) joined from 3.94 m and hit twice.
- **Verdict: PASS** on the pre-registered bar (≥1 conflict, ≥5 round polls, ≥3 hits; arm 0 ≤1 conflict; hits closer than
  the starts), in the host-fights-first scenario.
- **n = 1 per arm.** The flag stays **default 0** until repeated runs and a soak.
- Not tested:
  - guest-side life;
  - the host-idle scenario with the fixed build;
  - the sight stimulus 8 (the flag posts only 257 by default).

## 8. Stealth: can guards detect the puppet first?
**Static:**
- The detect reaction's start `sub_131EF30` sets its target `+0x74` to `sub_70E810(world)`, the world's player, and its
  `+0x64` to the entity at `*(*(this+0x20)+0x88)` (jobs v11/v21).
- `src/guard_sight.cpp` (`[Coop] GuardSeesPuppet`, installed only when the key exists, re-read every 2 s) redirects the three
  GetPlayer call sites that choose a sight / detect target:
  - 0x0103D61D (create);
  - 0x0103D7A1 (retarget);
  - 0x0131EFD5 (detect start).
  Each picks the nearer of player and puppet, with 3 m / 2 s hysteresis.
- The live ini has **no** `GuardSeesPuppet` key, so it is not installed today.
- New probe: `sub_131EF30` is wrapped (original first). It logs who the detection is about (`+0x64`) and who it targets
  (`+0x74`).

**Pre-registered A/B (written before the run):**
- Rig `stealth_ab.ps1`, same build per arm, `PuppetFightStim=0`.
- Phase S (50 s): the guest **walks** to the soldiers (36,-75) and stands there with no attack, while the host player stays
  at the spawn (~40 m away).
- Phase C (30 s): the guest walks back and the host player walks to the same spot (positive control).
- Read on the host, split at the phase boundaries.
- **Detected first** = during phase S, before any `[FI] apply enter`, at least one of:
  - a sight stimulus (id 8) or detect start whose source / about is the PUPPET;
  - a fight-start reaction or `StartConflict` on the puppet.
- Per arm, report it with its target (PUPPET vs PLAYER).
- **PASS for GuardSeesPuppet=1:** detected first with target PUPPET in arm 1; in arm 0 either nothing, or detections about
  the puppet that target the PLAYER (the GetPlayer behaviour).
- **Control valid:** phase C shows a detection about the PLAYER targeting the PLAYER in both arms.
- **INVALID:** no control detection.

**Result: INVALID. The soldiers at (36,-75) are not hostile on sight to anyone.**
- Asi `3495171d…`. Arm 0 host 31144, arm 1 host 20316. Both walks arrived within 1.5 m of (37.4,-74.9), 20-25 s each.

| arm | phase | FI enter | sight stim 8 (pup/pl) | detect starts (about → target) | reactions on puppet | conflicts (pup/pl) | hits |
|---|---|---|---|---|---|---|---|
| 0 | S (guest walks in) | 0 | 0/0 | 1 × npc → PLAYER | 026A45A8, 026A0A10 (stim 105) | 0/0 | 0 |
| 0 | C (host walks in) | 0 | 0/0 | none | none | 0/0 | 0 |
| 1 | S | 0 | 0/0 | none | none | 0/0 | 0 |
| 1 | C | 0 | 0/0 | none | none | 0/0 | 0 |

- **The control failed.** The host player standing beside the same soldiers raised no sight stimulus, no detection and no
  conflict.
- `[Sight]` in arm 1:
  - `retarget=212 puppetPicks=0 detect=0`: the redirect ran and never chose the puppet;
  - `hostile(G,P)=0 hostile(G,player)=0`: no probed guard counted either body as hostile.
- So this spot measures nothing about detection. AC4 soldiers there react only once provoked: restricted zones or
  notoriety.
- **Consequence (static, from §1 and §5, UNVERIFIED live):** the puppet carries the **player's own faction** (`FFADF4C0` both
  bodies). Its on-sight hostility on the host therefore follows the **host's** zone and notoriety state, not the guest's.
  A guest who is wanted in their own game is not wanted on the host.
- Not established: whether the one detect start above (about an npc, targeting the PLAYER) relates to the puppet.

**Notoriety rerun, pre-registered (written before the run):**
- Same rig with `-Notoriety`.
- **Phase P (provoke):** the host player attacks the soldiers for about 8 s, runs back to the spawn (about 40 m) and waits
  10 s. This gives a hostile context **on the host**, where the puppet carries the host player's faction.
- Then phase S and phase C as before.
- **Context valid:** phase P or C shows host-side hostility to the PLAYER (a detect start about the PLAYER, a stim-8 from the
  PLAYER, or a conflict on the PLAYER).
- **Detected first (phase S, before any `[FI] apply enter`):** a stim-8 / detect start about the PUPPET, or a fight-start
  reaction / conflict on the puppet.
- **PASS for GuardSeesPuppet=1:** detected first with target PUPPET in arm 1; arm 0 either none, or about the puppet
  targeting the PLAYER.
- Phase S runs while the host player is about 40 m away. A detection about the PLAYER in S is counted separately; it is not
  a puppet detection.

**Correction (ini):**
- The live ini **does** contain `GuardSeesPuppet=1`, and has since at least 11-15: `AC4Coop.ini.f7-session.bak`,
  `.coop-launcher.bak` and `.launcher.bak` all carry it.
- The earlier "no key" came from a search for `sight`, which cannot match that name.
- So `GuardSeesPuppet` was **on** in every run today that did not set it: §5-7, A/B rounds 1-3 and runs 1-8. Those results
  hold *with* the sight redirect.

**Result** (asi `3495171d…`; arm 0 host 37636, arm 1 host 25012). The guest never attacked: `[FI] apply enter` = 0 in
every phase of both arms. Counters are the last `[Sight]` line of each phase (cumulative).

| arm | phase | stim 8 (pup/pl) | detect start (about → target) | reactions on the puppet | conflicts pup/pl | hits pup/pl | sight retarget, puppetPicks | onVis hostile(G,P) |
|---|---|---|---|---|---|---|---|---|
| 0 | P (host provokes) | 0/0 | none | none | 0/5 | 0/8 | 167, 0 | - |
| 0 | S (guest in view) | 0/0 | none | none | 0/0 | 0/0 | 98, 0 | - |
| 0 | C (host walks in) | 0/0 | none | none | 0/0 | 0/0 | 212, 0 | - |
| 1 | P | 0/1 | none | none | 0/5 | 0/8 | 145, 0 | 0 |
| 1 | S | 0/0 | none | none | 0/0 | 0/1 | 101, **101** | rising to **26** |
| 1 | C | 0/0 | **1 × npc → PUPPET** | 105, 13, 18, **257 → 026B1BF0** | **1**/0 | 0/7 | 158, 1 (1 switch) | 1-14 |

- **Context valid:** phase P made the soldiers fight the host player in both arms (5 conflicts, 8 hits).
- **Arm 0 (GetPlayer behaviour):** the puppet stood in front of alerted soldiers for 40 s. Nothing at all was aimed at it:
  no sight target, detection, reaction or conflict.
- **Arm 1 (GuardSeesPuppet=1):**
  - In S, every sight retarget (101/101) chose the puppet, and guards' sight counted it hostile, rising to 26. But **no
    detection escalated within the 40 s**: `detect=0` calls at 0x0131EFD5, and no reaction or conflict.
  - In C a detect start fired **targeting the PUPPET**, and the guard ran the fight-start reaction on 257 and started a
    conflict on the puppet. The guest still had not attacked.
- **Verdict:**
  - Pre-registered bar (detected first *in phase S*): **FAIL** in both arms.
  - With `GuardSeesPuppet=1` the puppet *can* be detected and engaged without attacking, as in phase C, but it did not
    happen within S's 40 s. Phase C differs in two ways: the guest was walking away, and the host player approached.
  - Without the flag, it never happens.
- n = 1 per arm.

**RETRACTION of the notoriety result above.** The host player **died in phase P in both arms**: life 125 → 0 at the P/S
boundary, a respawn and a new puppet entity in the middle of phase S (`[PL]` lines of hosts 37636 and 25012).
- Phase S therefore spanned a death and a world reload, and its "seen hostile, no detection in 40 s" reading is void.
- What survives:
  - with the redirect off, no sight target, detection or reaction was ever aimed at the puppet;
  - with it on, one detect start targeted the PUPPET and escalated (stimuli 13, 18, 257 → fight-start → conflict). In the
    same spot in arm 0 the host player was not engaged.

**Guest-provoke rerun** (asi `3495171d…`; hosts 9208 / 28032; nobody died; life min 125 on both sides): **inconclusive.**
- The scripted 6 s guest attack gave 1 `[FI] apply enter`, 0 attacks, 0 conflicts and 0 hits, so no hostility was created.
  The rig also forced `PuppetFightStim=0`.
- Arm 1 phase S: sight chose the puppet 96/96, with VIS=36, and still no stimulus-8 selection and no detection.
- This is consistent with calm guards ignoring the sight of a non-hostile player-kind body. The intake `sub_15B6AD0` →
  `sub_15AE4F0` drops by reaction priority and faction; it was read, not traced live.

**Owner-driven pass** (asi `3495171d…`, host 36040, guest 22188; `GuardSeesPuppet=1`, `PuppetFightStim=1`):
- The owner played the guest: attack a soldier, flee, wait, walk back in without attacking, stand about 40 s.
- Fight phase, on the host:
  - `[FI] apply enter`, then `[PFS]` posted 257 to guard `3AB85790`;
  - that guard ran the fight-start reaction and `StartConflict(guard, PUPPET)` from 2.00 m;
  - a second guard joined on stimulus 135 from 13.27 m;
  - the puppet got a round, with 2 teams;
  - 1 guard hit at 1.42 m, relayed by PDR (the guest's life went 125 → 105, then regenerated).
- **After the guest left the fight (`[FI] apply exit`), the host never ended it:**
  - the puppet's round `3AB9B790` and both teams stayed until the end of the log, about 5,400 lines later;
  - guards kept calling `StartConflict(guard, PUPPET)` from 36-46 m: 76+ calls, each with `a2round=0`;
  - **no further hit landed**: 1 hit in the whole pass, including the 40 s stand next to the soldiers.
- Sight: 54 retargets, all on the puppet, but cone=0, VIS=0 and range=0. The sensors that evaluated it were never in range.
- **Reading:**
  - On the guest's game the guards calmed and the guest walked back in. On the host the same guards were still in a conflict
    with the puppet, but were not acting on it. **Guard conflict state diverged between the two games.**
  - "Detected first" could not be observed: the host never returned to a calm state to detect from.
- Not established:
  - why the round never ends on the host (no fight-exit path runs for the puppet's team; `sub_1621670` was not seen);
  - why enlisted guards stop attacking once the reposts stop. This matches the 11-14 measured fact of guards enlisted on
    the puppet that neither move nor attack.

- Not established:
  - why S's hostile sightings did not escalate (detection timer, line of sight, search state);
  - whether C's detection needed the host player's approach.

## 9. Why the host never ends the puppet's fight: two of our own flags
**Cause (code + ini read, 2026-11-17):** the live ini has `ConflictKeepPuppet=1` and `FightKeepOnConflictEnd=1`
(`src/guard_poi.cpp`, from the 11-14 F7 work).
- `ConflictKeepPuppet` replaces the call at 0x017C013B (`sub_1608F70(fm, player)`, "some fighter targets the player") and
  answers yes whenever the FightManager's target record (`fm+0x304`) is the puppet. The engine's "left the fight" test
  therefore never runs.
- `FightKeepOnConflictEnd` wraps the transition action `sub_17895F0` and skips the target drop while that record is the
  puppet.
- Both were added when the engine dropped the puppet every 1-2 s. With native conflicts now starting (§5-7) they keep the
  fight for ever, which is §8's owner-pass result.

**The engine's end test** (`sub_17BFF50`, job w5; `sub_1608F70` / `sub_16090C0` decompiled in job w4). E50 + E20 are posted
when all of these hold:
- every fighter of the player's group has a target (`sub_16090C0`);
- none of them targets the player (`sub_1608F70`);
- `sub_17A9C30` is false;
- no entity of the list at `v3+0x250` is within the squared radius `*(float*)(dword_4DE6D48 + 0x2AC)` of the player.

**Correction to the old code comment:** `sub_1608F70` reads the fight group from `*(*(e+0xE8)+0x98)+0x10` (the round's
team), not from `+0x94`. The puppet has a round once a native conflict starts.

**Change: value 2 for both flags** (values 0 and 1 unchanged; the ini is left at 1).
- The puppet's fight is kept only while a member of the target's fight group whose own fight target is the puppet stands
  within the engine's radius of the puppet (`PuppetStillEngaged`).
- Otherwise the stub returns the engine's own answer and the drop action runs.
- If the tuning value or the group is unreadable, it keeps (value 1 behaviour).
- **Built (asi `664c1fa5…`), spine gate OK, NOT run in game.**
- Pre-registered check, with both flags at 2 and the owner driving the guest: attack, flee beyond the radius, wait.
  - **PASS:** `[Poi] ConflictKeepPuppet=2: … conflict end allowed` appears; the puppet's round and the team list clear
    (`pupRound=0`, `teams=0`) within about 10 s of the last engaged guard falling outside the radius; and a guard that
    stays in range keeps the round.
  - **FAIL:** the round persists as in §8, or the fight is dropped while a guard is still hitting the puppet.

**Result: PASS (n = 1, owner-driven).** Asi `664c1fa5…`, host 34352, guest 23164, both flags = 2 (set in the ini after the
copies started; both are re-read every 2 s).
- **Fight held while engaged:**
  - `[FI] apply enter`, `[PFS]` posts, `StartConflict` on the puppet from 3.34 m and 1.68 m;
  - the puppet's round `3E7E8880` with 2 teams stayed through the whole fight;
  - 3 guard hits landed at 1.48-1.63 m;
  - no `[FightRm]` for the puppet in that time.
- **Fight ended after the guest left:**
  - `[Poi] ConflictKeepPuppet=2: no fighter near the puppet - conflict end allowed`;
  - then `[FightRm] PUPPET role=7 from=0161F870 <00A00786 <00A00EB0`, the engine's own drop action on the deferred
    statechart queue.
- **One re-start:** guards 17.6 m away started a new conflict (round `39432980`), and it was released again about 0.8 s
  later by the same path.
- **Then calm:** `teams=0 pupRound=0` for 36+ one-second polls, to the end of the log.
- Timing was not measured against distance. There were about 10 one-second polls between the guest's last fight exit and
  the first release.
- After the release there was no new detection, stimulus-8 selection or conflict on the puppet. It is not known whether the
  owner did the optional walk-back.
- The live ini was restored to 1 / 1 after the run.

### Next
- Native emit, behind a flag defaulting to 0: make the puppet emit the fight-trigger stimuli where the player's actions do.
  - The emit sites are in `player-stimuli-2026-11-13.md`: 135/136 `sub_10608A0`, 22 at 0x0189BB38, 153 broadcast; 257 and
    8 not yet sited.
  - Or emit them from the replayed action stream (attack / draw / sprint), mapped from the run 7 id → reaction table.
- A positions check that guards close on the puppet, and a guest-side life read for the relayed hits.
- Name the teardown: `FightProbe` (RemoveFighter `sub_161F420`, with its call chain), with PuppetStim firing 8/257 from the
  puppet.
- Make the puppet emit the player's fight-trigger stimuli natively: the emit sites are in the table of
  `player-stimuli-2026-11-13.md`. Behind a flag, default 0.
- Find who installs the reaction at `this+0x88` and its target at `this+0x80`, and on what condition it picks `0x026B1BF0`.
  - Lead: the reaction selection in `perception.md`. `CanReact` is `sub_1076110`, and the brief records that it is not
    reached while a fight is active.
- Compare player and puppet at that selection point in one run, with the same rig.

## 10. Unified build on `feat/launcher`: confirming pass
- Commit `e6f97fa` ports PuppetFightStim (default 1) and value 2 of the keep flags onto `feat/launcher`; asi `62c7e5c7…`.
- Owner-driven pass, host 37776, guest 23888; the ini has both keep flags at 2, `FightIntents=1`, `IntentLane=1`.
- **Fight start:** 2 `[FI] apply enter`; 7/7 `[PFS]` posts succeeded; 4 `hit-post puppet` lines; 2 hits relayed by PDR and
  2 `owner apply hit` on the guest.
- **Fight end:** after the guest's last `[FI] apply exit`, 3 x `[Poi] ConflictKeepPuppet=2 … conflict end allowed`, each
  followed by `[FightRm] PUPPET role=7 from=0161F870` (the engine's drop action); then no further fight lines for about
  1,000 log lines, to the end of the run.
- 0 exceptions; both copies alive at the end.
- **Verdict: PASS (n = 1 on this build; 2 passes in total with §9).**
- Not measured on this build: the round / team state (the `[PL] gate` probe is not on this branch) and release time
  against distance.
