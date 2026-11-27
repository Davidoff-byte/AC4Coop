# Pose sync (F3) - what is built (`src/pose_sync.cpp`, proto 17 'AC4P')

Design and receipts: `pose-sync.md`. Re-verified in IDA session 4b0f0b4a (AC4BFSP.exe md5 2058342866688f780c8b34526a65bc35)
on 2026-11-27; `AC4BFMP.exe` not opened. Flags: `[Coop] PoseSync=0` (capture + send + receive + metric),
`PoseSyncApply=0` (the write), `PoseSyncApplyTrans=1`, `PoseSyncHz=10`, `PoseSyncBlendPct=100`, `PoseSyncMaxAgeMs=500`,
`PoseSyncLoopback=0` (solo: feed our own frames to our own puppet). PoseSync=0: no patch, no `[Pose]` line, and an
'AC4P' datagram takes the old unknown-magic path (ignored).

**Seam (verified).** `0x45349C call sub_769150` in `sub_453330` (only caller `0x453763` in slot-44 `sub_453740`, which
holds the component lock `+0x104`). ESI = SkeletonComponent (`0x453331 mov esi,ecx`, never rewritten), ECX = LOCAL
(`0x45348E mov ecx,[esi+134h]`), after the anim graph and `sub_44FB70` IK, before the LOCAL->WORLD compose. The call is
redirected (bytes `52 50 E8 AF 5C 31 00 5F 5E C3` at `0x45349A` checked at install and in `verify_signatures.py`) to a
thunk: pushad, handler(ESI, ECX), popad, jmp `0x769150`. Sender captures here for the player's comp; receiver measures
and (Apply=1) writes here for the puppet's comp. Engine calls: `sub_44DB80` (thiscall comp, crc32; retn 4; AL, 0xFF =
miss), `sub_758C30`/`sub_75A2E0` get rot/trans (thiscall pose, idx, float4* 16-aligned; retn 8),
`sub_759060`/`sub_759250` set (retn 8). None takes a lock; their only calls are on the root path, which we never reach
(we require LOCAL `+0x8C` bit 27 set, else count `rootBit0` and skip).

**Wire.** `PoseWireHeader` 20 B {magic 'AC4P', protoVer, seq, hostMs (QPC ms), u8 kind=1, u8 count, u16 0} + count x
`PoseWireRecord` 18 B {u32 crc32(name), u8 flags (bit0 = t valid), u8 0, u8 q48[6], i16 t[3] x2048}. q48 =
smallest-three (2-bit dropped index + 3 x 15 bit). 24 bones -> 452 B/frame, 4.5 kB/s at 10 Hz. Receive rejects: proto,
kind/reserved, count 1..63 with exact length, reserved record bits; newest seq wins (`stale` counter).
Codec self-check at install: 2000 random quats, `q48maxErr` logged (offline harness, 200k quats: 0.0074 deg).

**Bones** (HumanIK, all but Hips decoded from the engine table `0x1E70040`): Hips(+t), Spine, Spine1, Spine2, Neck, Head,
{Left,Right}{Clavicle, Arm, ForeArm, Hand, HandMiddle1, UpLeg, Leg, Foot, FootThumb1}. Indices are resolved through
`sub_44DB80` per rig and per tick, never cached. Reference (root) is not sent.

**Log.** `[Pose] PoseSync=1 installed ...` (first tick), `[Pose] tx rig comp= resolved=M/24 mode= missing=[...]` (per
sender rig), every 5 s: `[Pose] matched=M/N err p50= p95= native p50= p95= samples= apply= puppet= unmatched=[...]` and
`[Pose] tick hook cap= app= ... rootBit0= clampT= old= ownerMiss= exc=`. `err` = received vs the LOCAL handed to the
compose (after the write when Apply=1); `native` = received vs the puppet's own animated LOCAL (before the write).
One sample set per received frame.

## PASS / FAIL (run on the receiver unless stated)

| # | check | PASS |
|---|---|---|
| C0 | control PoseSync=0 | zero lines starting `[Pose]`; `[AC4Coop] CFG Coop.PoseSync=0` |
| P1 | PoseSync=1, same rig (2PC or Loopback=1) | `matched=N/N` with N >= 20, `samples > 0`, p95 printed (not -1) |
| P2 | sender rig | `tx rig ... resolved=M/24` with M >= 20; missing names listed |
| P3 | Apply=0 baseline | `err` == `native` (no write); record p95 = E0 |
| P4 | Apply=1, BlendPct=100 | `err p95` < 0.5 deg and < E0; `appliedTicks` rising; `rootBit0=0/0 exc=0` |
| P5 | visible (weakest: "I looked") | puppet arms/legs follow the peer in fight/blend poses where alias replay diverged |
| P6 | safety, 10 min Apply=1 | no crash, `exc=0`, `clampT=0` (else Hips t clipped at +-16) |

FAIL = any row not met. P4 measures what the compose consumes, not pixels; P5 carries the render claim.

## UNVERIFIED

1. That the renderer/skinning consumes the WORLD buffer this compose writes (`sub_42B970` publish; consumer not read).
   Apply is therefore behind its own flag: **NEED RE** for the consumer, or P5 live.
2. Which thread runs slot 44 (data xref only). Handler is thread-agnostic: TryEnter on private locks, no log/alloc.
3. Later writers of LOCAL/WORLD in the same frame: the IK job `0x453780` (`sub_442460` root z via `sub_43AE00`, foot IK
   `sub_4CF4A0`) and recompose `sub_43AE80`. Captured pose excludes them; they may still move the puppet after our write.
4. That the anim graph rewrites every LOCAL bone each tick (decides whether BlendPct<100 is a fixed blend).
5. That the puppet's rig resolves the same 24 names (logged as `matched`/`unmatched`), and LOCAL bit 27 stays set.
6. Hips LOCAL translation within +-16 (packed scale); `clampT` is the signal.
7. Rel32 write at install is not atomic (5 bytes); done at DLL init like the other detours.
