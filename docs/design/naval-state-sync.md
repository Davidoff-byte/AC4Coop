# Naval state sync — flags of `feat/naval-state-sync` (`src/phase1.cpp`)

All `[Coop]`, all default 0, all read once at first use. Evidence: `ops/evidence/naval-boarding-noclass-sink-2026-11-27.md`
(parent repo). Merged into `feat/integration-1121` 2026-11-28 (`c154100`); **no live run yet** for any of them.

## ShipClassFix (host and guest)
- `GetShipEntity` / `FindLocalShipByObjId` apply the engine's own validity mask to a naval-list handle
  (entity = `h[0] & (h[2] >> 31)`, IDA `0x10968C0..0x10968CA` in `sub_1096870`): an entry with the sign bit clear is null.
- `fillShipState` writes the class (`ReadShipClass`, byte for byte `sub_11AA850`) into `arch` on every ship record; for
  class 0 the max hull (`GcLShip+0xB4`) rides `actionAlias` as float bits.
- PASS: `[ShipClassBind] mode=2 … noClass=0` on the guest while host ships are bound.
- Negative control: ShipClassFix=0 with ShipMirror=0 and ShipClassBind=0 on the host — the records carry `arch=0`.

## ShipClassProbe (host, read-only)
- One `[ShipCls] obj=… i=… h2sign=… … cls=… src=gate|fallback|none … fix=…` line per naval-list object id (512 ids, then
  silent); reads the unmasked handle on purpose, so `h2sign=0` entries are visible.
- PASS: the local Jackdaw's line has `cls=7`.
- Negative control: ShipClassProbe=0 — no `[ShipCls]` line at all.

## ShipStateSync (host captures, guest applies)
- Host: `stage` = health state 91..95 as 1..5, `hp` = hull per-mille (1001 = no hull).
- Guest `NavStateFire` (OnFrame, after the `OnFrameInner` `__try`, player-update thread) posts the engine's own statechart
  event (`sub_A014A0`): host 95 / local 91 → `sub_1207960(gcl,1)` then `*(u16*)0x4DEAD5C`; host 94|93 → `*(u16*)0x4DEADE0`.
  Hull through the setter `0x1225A10` + regen cap, clamped so the drive itself never posts 95 or the sink.
- PASS: `[ShipSt] PASS key=… host=… local=… dtMs=…` after a host state change; `[ShipSt] stats … pass=` rising.
- FAIL / alarms: `[ShipSt] FAIL mismatch key=…`, `[ShipSt] UNSANCTIONED sink key=…`.
- Negative control: ShipStateSync=0 — `NavStateFire` returns at once (no `[ShipSt]` line); host records carry `stage=0`.

## ShipNoAutoSink (guest; code patch)
- Redirects the call at `0x01205379` in `sub_1205280` (state 95's > 1000 m sink post) to `NavNoAutoSinkPost`, which drops
  the post for a ship in the ShipStateSync table and forwards every other. The 10 bytes at `0x01205374` are checked first
  (`tools/verify_signatures.py` "ShipNoAutoSink call").
- PASS: `[ShipSt] ShipNoAutoSink: call 0x01205379 -> …` at install; `[ShipSt] stats … noAutoSink skip=` > 0 when a
  host-owned copy in state 95 is > 1000 m from the guest's Jackdaw, and the copy does not sink.
- Negative control: ShipNoAutoSink=0 → nothing installed (no line); bytes differ → `[ShipSt] ShipNoAutoSink: bytes at
  0x01205374 differ - NOT patched` and no redirect.
- Open: the evidence says the reference point is the guest's own Jackdaw (ship-manager slot `+0x1674`); copies entering 95
  only within 1000 m of it would need no patch.

## BoardStartGuest (both)
- BoardProbe's S2 and BoardingForce queue the ship's GcLShip; `BoardStartGuestFire` (OnFrame, outside every `__try`)
  enters state 95 the engine's way — `sub_1207960(obj,1)` then `sub_A014A0(obj, *(u16*)0x4DEAD5C, 0)` — only from state 91.
- PASS: `[Board] S2e obj=… evt=… before=… after=… bit91=0 bit95=1 …`, then `[Board] S2e+5s obj=… regenStopped=1`.
- Negative control: a ship not in 91 → `[Board] S2e obj=… state=… - not 91, nothing posted`; BoardStartGuest=0 → the old
  raw bit write (`[Board] AFTER …`).

## Not built
- BoardObjectiveReplicate (evidence Q1d: activate missing slots with `sub_12DBDB0(battle, obj)`, complete with raw
  `State = 2`): its calling convention is not written in the evidence → NEED-RE before code.
- Guest cannon damage can still sink a bound copy (candidate "unsinkable" bit `+0x160` bit 3; other writers unknown).
