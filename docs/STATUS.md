# Status

Last updated 2026-11-11, at `main` after PR #1 (the live-tests stack). This file replaces every plan,
journal and handoff in `docs/archive/`. Update it in the same PR as any change that alters what is true here.

## Build
- Target: `AC4BFSP.exe` PC 1.05, 45,056,040 B, sha256 `bc11ad79…ab5150`. Any other build is refused by the
  build gate (`known_builds.txt`), which logs `UNKNOWN_BUILD` and patches nothing.
- Last pinned build: `cddbc41`, ASI 342,528 B. `main` is now three commits ahead of it (skel-attach `1eb652d`,
  F8 BoardableTest `19788a1`/`9f6fe53`) plus README updates.

## Verified, and how
| area | state | verified by |
|---|---|---|
| transport | UDP peer-to-peer both directions, ~200 ms over a VPN | two-machine run |
| player puppet | the peer's body mirrors position and animates; collision off; ~33 kB/s | live run 2026-11-10 (`glide-live-2026-11-10`) |
| kills | host→guest | live; guest→host is built but unverified |
| enemy scope | classified by `entity+0x9C` kind/role from the game's own enums | `[Fight]` census (`d4b5088`) |
| ship mirror | a duplicated local ship carries the peer's hull HP (`GcLShip+0xD0`) and transform | live |
| body spawn | engine `EntityBuilder` + duplicated `RowSelector` spawns a second Edward | in game |
| event layer | exactly-once delivery under 30 % loss | `tools/event_peer.py` selftest (`05eb0c6`), Python only |

## Measured negatives
- Guards do not perceive the other player's body, with or without collision. `docs/ops/P6-RESULT.md`, `RUN-2026-11-10-X2b`.
- A guard-shaped spawn through the crowd route (`EntityBuilder` slot 39) access-violated 3/3.
- The 2026-11-08 launch crash was not gated by any ini flag; its root cause was the action-guard trampoline
  continuation jump, fixed in `c718e92`. The investigation is in `docs/archive/ops/HAUL-2026-11-08.md`.

## Built but not verified live
- Everything after `cddbc41`: skel-attach (`[Coop] SkelAttach`, default 0, tested solo only) and BoardableTest
  (`[Coop] BoardableTest`, default 0).
- Guest→host kill relay.

## Open
- Guard perception of the other player: needs the Gamer/avatar route (`docs/ops/ANGLE-GAMER-AVATAR.md`,
  `docs/ops/GAMER-AVATAR-API.md`); two PC-mapping anchors for the Gamer global came back negative (`b5fe59b`).
- Sail/billow sync: spec only, needs protocol 14.
- Second game build (1.08 etc.): 22 of 101 sites relocated (`docs/archive/ops/JOURNAL-2026-11-04.md` §11).
- Connection setup is manual: both machines edit `[Net] PeerIP/LocalPort/PeerPort` in `AC4Coop.ini`, and
  internet play has needed a VPN.
- The spine's byte arm needs the pinned compiler `cl 16.00.40219.01`; until then no unit can reach `matched`.
- No CI, and no committed pre-commit hook; `tools/spine_gate.py` is run by hand.

## Plan of record
`docs/ops/PLAN-COOP-V2.md` (outcome-sync architecture, gates P0–P8), with `docs/ops/SOLO-PLAN.md` for what one
machine can prove.

Next slice: lobby codes (host shows a code, guest enters it), serverless, one guest. Design and PR stack in
`docs/design/lobby-code.md`.
