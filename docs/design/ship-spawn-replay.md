# [Coop] ShipSpawnReplay (slice 1): engine-spawned enemy ships on the guest

RE + full design: `ops/evidence/ship-spawn-replay-design-2026-11-27.md` (s1-s7 and "Addendum - risk closure"). Code:
`src/ship_spawn_replay.cpp`, hooks in `src/spawn_census.cpp`, binding in `src/phase1.cpp` `ShipClassBindFor`.

## Flag
`[Coop] ShipSpawnReplay` default 0. Non-zero at load installs DetFac (0xF72ED0), DetSrc (0x79C6C0) and DetSet (0x5BC470).
After that it is re-read once a second. Needs `ShipClassBind` 1 or 2 (2 moves the hull) and `DisableSpawns=0`, because
the DisableSpawns hook on 0x79B8D0 also declines our request.

## Flow
1. Capture (manager job thread, DetSet entry, plain reads only, no engine calls, no locks): ship entity (vt 0x01E4A128)
   whose spec has a first params of vt 0x0270A538 -> ring {req, ent, spec, owner, ctx, p4C}.
2. Class table (player thread): when the entity is in the naval list, `specFor[ReadShipClass]` = the newest tuple.
   Classes 0, 7, 8-16 are skipped. The table is cleared on world change (next to `MirrorRetireAll("world-change")`).
3. Queue: in `ShipClassBindFor`, after the free-hull search and the steal both fail (`noLocalOfClass`),
   `{hostObjId, cls}` is queued, one entry per host id.
4. Fire: called from `OnFrame` after the OnFrameInner `__try`. It runs on the player thread only, with no `__try`.
   Limits: 1 request in flight, at least 2000 ms between fires, at most 6 replayed ships alive, and a class is given
   up for 60 s after 3 fails. Before the call: VirtualQuery checks, spec and owner vtables as captured, params count,
   params vt. Then `sub_569560(spec, owner, ctx, 0, 0)`. The request comes from DetFac with a thread tag, and
   afterwards `CAS(spec+0x3C, ourReq -> old)` restores the original request.
5. Bind: the built entity must be in the naval list and `got == want`. The hull is then reserved for that host id and
   bound in `ShipClassBindFor`. When `got != want`, the hull goes to a queued host of class `got` if one exists,
   otherwise it stays unbound (`classMiss`). Ordinary binding and steal skip hulls reserved for another host.
6. There is no mod despawn. A reservation lapses 10 s after its host stops asking, and the hull becomes a normal free hull.

## Thread / lock rules
- Do not call into the engine from DetSet. DetSet is on the job thread, inside the client's locks.
- `sub_569560` is called only from `ShipSpawnReplayFire`, never under `__try`. It reaches EnterCriticalSection(client+4)
  at 0x5BCAE0 on the caller's thread.
- Do not poll `req+0x20/+0x34` from the player thread, because the request lifetime is unknown. `failed` = no built
  entity in the naval list within 10 s (this includes state 9).

## Oracle (exact log predicates; ratios are taken over cumulative `stats`)
| # | PASS | FAIL |
|---|---|---|
| O1 | final `stats`: `built/fired >= 0.9`, and every `req ... submitted=1` is followed within 10 s by `built host=<same>` | `< 0.9`, a `[Frame] EXCEPTION`, or a crash |
| O2 | `classOk/built >= 0.95` (`built` lines with `want == got`) | `< 0.95` |
| O3 | some `stats` line with `boundTotal > localAI`, `[ShipClassBind] noLocalOfClass` falling >= 50 % against the flag-0 control on the same route, and the mean `err` of `bind` lines `< 5` m | otherwise |
| O4 | measured: `gone` per 5 min (replayed hulls that left the naval list without any mod action) | - |
| O5 | both copies alive 10 min at sea, including 1 fast travel with `pool > 0` (`clear (world-change)` line present) | a hang or crash; a dump with `RtlEnterCriticalSection` = FAIL |
| cap | each class logs one `[ShipSpawnReplay] cap cls=... pB=...` line (pB = `*(params+0x4C)`, instrument only) | - |

## Negative control (flag 0)
- No `[ShipSpawnReplay] req`, `cap`, `built`, `bind` or `stats` line appears.
- `hooks=` is not printed either, unless the flag was non-zero at load.
- The `[ShipClassBind]` counters keep their old meaning. Every new branch is gated on `ShipSpawnReplayOn()`, which is
  false when the hooks are not installed or the flag is 0.
- The `SpawnReplayFire` mode-1 fix is separate: it changes only mode 1, which never matched before.
