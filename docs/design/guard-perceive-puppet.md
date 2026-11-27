# [Coop] GuardPerceivePuppet (default 0)

Spec: `ops/evidence/guard-perceive-puppet-design-2026-11-27.md` (AC4BFSP, sections 2.2 and 3.1). Host only. Needs
`GuardSeesPuppet` (installs the patches); read live with it every 2 s. No new patch site and no engine call: only the
decision behind the existing `guard_sight.cpp` / `guard_poi.cpp` patches changes. With the flag at 0 the old code runs.

## What it does

| gate | site | flag 0 (old) | flag 1 |
|---|---|---|---|
| G1 create | `0x103D61D` | nearer body | unchanged (nearer body, decided once) |
| G1 retarget | `0x103D7A1` | nearer body, 3 m / 2 s hysteresis | `req=*(los+0xF8)`, bit 2 of `u16 req+0x10` = VISIBLE for the previous body: visible -> keep; else hold the body last seen for 1.5 s; then alternate the two bodies, each >= 0.5 s in the slot |
| G3 detect start | `0x131EFD5` | sight-table body (`DetectKeepTarget` keeps only a puppet `+0x74`) | the body `CRLDetect+0x74` already holds (CreateReaction `0x1055D12`) when player/puppet; else the real GetPlayer. `DetectKeepTarget` is not read |
| G4 | `0x6F6010`, `0x65A688` | lend puppet while the sight table tracks it | lend puppet only while that guard's G3 target is the puppet (< 3 s), or `GuardPoiFightTarget` |
| G4 | `0x1B06D51` | not patched | not patched (world-level flag) |

`SightProbe` is not fed while the flag is on (its faction getters are engine calls). Not implemented: design 3.1(b)
(other body within sight range) - it needs `sub_18779F0`, an engine call.

## Log lines (host)

* every 5 s: `[Perceive] on=1 g1 calls=N pickPup=N pickPl=N keepVis=N hold=N probeSwitch=N | g3 calls=N tgt pup=N pl=N other=N | g4 lend=N`
  (only when the flag is on; counters reset per line)
* `[Perceive] gate=1 guard=<G> vis=pup|pl after=<ms>ms` - a body became visible after >= 1.5 s unseen (or first time); <= 40 per run
* `[Perceive] gate=3 guard=<G> target=puppet|player src=stim74|getplayer` - every detection start; <= 20 per run
* guest: `[Perceive] guest guard=<objId hex> stage=<0|1|2> local=<0|1>` on change, <= 200 per run

## PASS / FAIL (host log, two-instance rig, same save, n >= 3 per arm; record asi sha256 + commit)

* **Control** (`GuardPerceivePuppet=0`, `GuardSeesPuppet` absent): PASS iff no line matches `^\[Perceive\]` and every
  `[Sight]` line (none expected) has `puppetPicks=0`.
* **Arm A** (flag 1; host player made hostile first, retreats 40 m alive; guest walks in front of guard G): PASS iff within
  40 s, in order, `gate=1 guard=G vis=pup`, then `gate=3 guard=G target=puppet src=stim74`, then the existing
  escalation / conflict line naming PUPPET for G; and no `gate=3 .* target=puppet src=getplayer` line exists.
* **Arm B** (flag 1; host player in front of G, puppet behind it within 3 m): PASS iff a `gate=1 guard=G vis=pl` line
  appears, after it no `gate=1 guard=G vis=pup` line while the player stays in view, every `gate=3 guard=G` line has
  `target=player`, and `[Perceive]` lines in the window have `keepVis>0`. Comparison arm: same layout, flag 0 (old rule
  expected to switch to the puppet: UNVERIFIED).
* **INVALID** if, in arm B, no `gate=3 guard=G target=player` line fires at all (the player control did not detect).

## Guest lane (proto 17)

Carried on `IC_ACTION` ([Coop] `GuardActionLane`, needs `IntentLane`), wireId = guard objId, published on the guard's
own update when alias / stage / target / perception changes. Payload = `GalPayload` (16 B, unchanged) + 2 bytes:

| off | type | field |
|---|---|---|
| 16 | u8 | perception stage: 0 none, 1 sight visible (committed body seen < 1.5 s), 2 detection start (< 3 s) |
| 17 | u8 | targetIsPuppet: 1 = the host's puppet = **the receiver's local player**; 0 = the host player |

The 18 B form is sent only with the flag on; receivers accept 16 B and 18 B. `NET_PROTO_VER` 16 -> 17 (intent golden
vector in `net.cpp` and `tools/lobby_peer.py` re-derived). The guest records and logs the bytes; nothing is applied to
its guards yet. ESCALATE / CONFLICT stages (design 4) are not carried: no hook observes them.
