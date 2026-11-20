# Boarding predicate — live confirmation (2026-11-20)

**Build:** `feat/sail-memento` `9e4b8fa`, ASI 757,760 B sha256 `13e99339…486757`,
stamp `commit=9e4b8fa tree=clean` on both copies.
**Rig:** two copies, one machine, `EntityHost` per copy. `BoardProbe=1`, `BoardProbeRange=20000`.
**Probe:** pick the nearest non-local ship, read the two `BoardingCondition` predicates directly, run
S1 (engine setter only) then S2 (setter + bit 95), re-reading after each. Hull is read-only.

---

## Result — both instances, identical

```
[Board] S0 ship=3930FA10 gcl=3932D6B0 hull=350.0 maxHull=350.0 a3A9=0 a540=0 bit95=0
[Board] S0 pred408=0 pred368=0 dist=918.5
[Board] S1 setter-only a3A9=1 a540=1 bit95=0  (expect a540=1, bit95=0)
[Board] S1 pred408=0 pred368=1
[Board] S2 setter+bit95 a3A9=1 a540=1 bit95=1 hull=350.0  (expect bit95=1, hull unchanged)
[Board] S2 pred408=1 pred368=1
[E] ev=boardprobe wrote=1 hullMoved=0 maxHull=350.0
```
(guest pid 21520 identical, different pointers and `dist=937.5`.)

## What this proves

1. **The two-predicate decode is correct.** `BoardingCondition` = `pred408 && pred368`, nothing else.
   `vtbl+408` tracked **bit 95** exactly (`0 → 0 → 1`); `vtbl+368` tracked **`+0x540`** exactly
   (`0 → 1 → 1`).
2. **`IsBoardable` (`+0x3A9`) alone is not enough.** S1 set `a3A9=1` and the condition stayed **false**
   (`pred408=0`).
3. **The engine setter alone is not enough either.** S1 set `a540=1`, `pred368=1`, and the condition
   still stayed **false** because bit 95 was clear.
4. **Setter + bit 95 makes the condition true with the hull untouched** — `hull=350.0` before and after,
   `hullMoved=0`, on a ship at full hull. So a mod can make an AI ship boardable without the hull
   threshold, which is what `boarding-threshold-2026-11-20.md` predicted statically.

## What this does not prove

- That the *player-facing* prompt appears — the probe reads the AI condition, not the UI. The earlier
  F8 run showed the prompt/board flow works when the setter produced the state; this run adds the
  condition algebra, not a new UI observation.
- Behaviour when the mask dword is shared or read-only. The probe's S2 has a `skipped: mask not
  writable/shared null` path; it was not hit here (`wrote=1` on both).
- Anything about co-op mirroring of boardable state (the open half of `OPEN.md` O11).

## Note on the probe's own reach

`BoardProbeRange=20000` was needed because the save has no non-local ship within 200 m — the first probe
run produced **zero** lines, and the "waiting" line added in `9e4b8fa` is what turned that silence into a
stated fact (`ships examined=0`) before a ship at 918 m qualified.
