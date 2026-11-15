# Crash soak: same-thread action writes OFF vs ON (2026-11-16 01:29-02:45)

Question from `crash-dumps-2026-11-16.md`: do the action-system crashes on worker threads go away when our action writes
happen on each body's own update? The levers are `[Coop] PuppetActionsOwnUpdate` (puppet) and `[Coop] MirrorActionsDeferred`
(guards), both default 0.
- Build: asi bbeabb58... (commit 4b3e9bc), D: Steam install, pinned exe.
- Runner: `soak.ps1` (session scratch). It writes the arm's values into AC4Coop.ini before `two_copy.ps1`, because both
  flags are re-read every 2 s and the runner's restore would otherwise switch them off mid-run. The original ini was
  restored at the end (sha256 equal).

## Protocol (each run)
- Two copies; common settings `NativeReplica=0 FightIntents=1 IntentLane=1` (so take-over and wide bind are on).
- Both players teleported to the soldier area (36, -74), `two_copy_moves.py` on the host, host back to the soldiers,
  60 s more. About 279 s per run.
- Arms alternate: odd runs OFF (both 0), even runs ON (both 1). Each guest log was checked for the flag lines (`pab=1 mad=1`
  in every ON run, 0 in every OFF run).
- Crashes are matched by pid against the Application log (event 1000) and the WER dump folder, and split into in-run (before
  the run's end time) and at-close.

## Result
| arm | runs | booted | host died in run | guest died in run | crashes at close |
|---|---|---|---|---|---|
| OFF | 8 | 8 | 0 | **2** (runs 7, 13) | 0 |
| ON | 8 | 8 | 0 | **0** | 0 |

Both crashes are guest `0xC0000005` with WER dumps, read with cdb (x86, no symbols):
- **18272 (run 7):** `eip=006781f4 mov edx,[eax+4Ch]` with eax = -1 (address 0x4B): an invalid object.
- **26568 (run 13):** `eip=0`, a virtual call through a null slot. Caller `0x006781FB`, the next instruction of the same
  function.
- Same worker-thread chain in both: `0x0067DAD7` ← `0x0067DC6B` ← `0x0073650D` ← `0x004C9161` / `0x004CAC5E` / `0x004CB443` ←
  **`0x00662648`** (action update) ← job dispatch. This is the chain of 14988 and 22296 (`crash-dumps-2026-11-16.md`).

## Reading
- All four dumps of this family so far (22296, 14988, 18272, 26568) came with both flags off. None appeared in 8 ON runs.
- On the soak alone, 2/8 against 0/8 is not significant: with 2 crashes in 16 runs, the chance both land in the OFF arm
  by luck is 8/16 x 7/15 = 0.23. It is consistent with the stack evidence (22296 crashed right after our puppet
  `SetTargetAction` write from the player's update).
- The cause stays a hypothesis: cross-thread writes into a body's ActionComponent. It is not proven.

## Animation with the flags ON (one run, ActMatchProbe=1, pids 26280 / 10104, two_copy_moves.py)
- Guest's puppet of the moving host:
  - ground (ctx 4): 95-100% in every steady window; one transition window 44.8%;
  - off-ground: ctx 6 82.6-90.1%, ctx 9 78.2-83.3% (one window 0/1), ctx 10 63.2-98.6%, ctx 30 87.5-90.3%.
- The OFF reference (`act-match-ab-2026-11-15.md`): ctx 6 86-96%, ctx 9 50-94%, ctx 10 57-100%, ctx 30 71-79%.
  Comparable; n = 1.
- Frames per 5 s window were about 130 here against about 300 in the 2026-11-15 OFF run. Whether that is load or the
  flags was not separated.

## Not checked
- Whether the ON arm adds visible lag to guard or puppet actions (the alias is applied one body-update later).
- The boot-time exits without dumps (10104, 27008, 29188).
- The heap-corruption assert (32460): one occurrence; no repeat in these 16 runs.
