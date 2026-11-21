# Integration build — coord sync + ship mirror together (2026-11-21)

**Branch:** `feat/integration-1121` @ `e749e37` (merge of `feat/launcher` @ `5f01f8a` into
`feat/shipmirror-port` @ `3539f8b`). ASI 819,712 B sha256 `f19b9d92…24605f`, in-game stamp
`commit=e749e37 tree=clean` on **both** copies. Host pid 32868, guest 30168.

## Why this run existed

Two branches had diverged — 10 launcher commits (combat lane, guard stage, GuardPlace) against 39
shipmirror commits (coordinator enum/sync, hull+sail, fort probes, ship mirror port). Every test was
measuring half the mod, and the deployed binary flipped between the two on each redeploy. The merge
resolved **one conflicted file, two semantic hunks**, both by union:

1. `FightIntentDrain` early-out — union of launcher's `GuardActionLane`/`OwnerHitStalePoll` and
   shipmirror's `SailSyncOn`/`CoordSyncOn`.
2. `OnFrameInner` tick list — union of both, `SpawnerIdProbeTick` once, `CombatTiming` block kept
   outside the `__try`.

`netstate.h` needed no manual merge: `IC_COORD = 12` and `IC_SHIP = 11` coexist, `IC_LIMIT = 16`, no
collision. `.vcxproj` union verified file-by-file.

## Result

```
guest [ShipMirror] stat live=8 pool=8 created=8 classMiss=0 thisPkt=6 samples=334 meanErr=0.00 maxErr=0.00
host  [CoordS] tx=20
guest [CoordS] rx=20
both  exceptions=0
```

- **Eight AI ships mirrored**, every proxy built from a same-class local source (`classMiss=0`), and
  positional error **0.00** across 334 samples.
- **Coordinator sync live in the same binary**: 20 publishes, 20 applies, no exceptions.
- The merge is behaviourally sound for both lanes — the union did not break either.

## What is NOT proven

- **Visual correctness of the proxies.** `meanErr` is the error between the host's record and the
  transform we write; it says nothing about whether the hull class, heading or sails *look* right.
  Needs the owner's eyes.
- **Soak.** The earlier mirror instability manifested as a crash after minutes, not seconds. 334
  samples is well under that; a longer session is the real test.
- **Interaction under load** — both lanes ran, but not during combat or a boarding.

## Provenance note (worth keeping)

The first build of the merge stamped `commit=3539f8b tree=dirty`: the build ran **before** the merge
commit landed, so the binary carried merged content under a pre-merge stamp. Rebuilt from the clean
`e749e37` and re-verified **in the logs** (`[Build] asi=… commit=e749e37 tree=clean`) rather than from
the file. Rule: the stamp in the running process is the only provenance that counts.
