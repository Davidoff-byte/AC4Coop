# SOLO PLAN — what one machine can prove

One copy of the game, plus `tools/peer9.py` (a Python client that speaks the real protocol) and
`tools/event_peer.py` (a Python reference implementation of the event layer). No second PC, no
second game instance, no build port.

This is the subset of `PLAN-COOP-V2.md` that does not need two rendered worlds.

| phase | solo? | instrument | gate |
|---|---|---|---|
| P0 link + ghost | **fully** | peer9 `--walk`, `--pos` | `rx>0` 10 min, `maxD < 0.35 m`, no crash |
| P1 same-world anchor | **fully** | peer9 `--world`, `--scene` | same ids 60 samples, distance < 2 km |
| P2 event layer | **fully** | `event_peer.py selftest` | 200 events @30 % loss → 200 once, < 300 ms |
| P3 combat parity | **half** | host kill → event → peer9 asserts | 20/20 delivered once; corpse *not* verifiable |
| P4 semantic identity | no | needs two spawned worlds | — |
| P5 enemy scope | **fully** | `[Sync] scope` + `kindrole` | within 5 % of a hand count, 0 civilians |
| P6 stealth perception | **fully** | AI's 3-minute experiment | guard stage changes with the ghost, not with control |
| P7 naval | **half** | host-side only | hull/transform parity unverifiable |
| P8 progression | no | needs two mission graphs | — |

## Order (highest information first)

1. **P6 — does stealth exist at all.** One run, three minutes. If red, an entire phase leaves the
   plan and the project gets simpler.
2. **P0 — re-establish the baseline.** Nothing has been verified since commit `ad1aea9`.
3. **P2 — reliable events.** The prototype exists and the clean-link case passes; the lossy case
   still fails (see below). This must be right before it is written in C++.
4. **P1 — same-world anchor.** A few lines of logging that protect every later diagnosis.
5. **P5 — enemy scope.** A census you can hand-count.

## P2 status, honestly

`tools/event_peer.py selftest`:

```
clean link   200/200 delivered, 0 duplicates          PASS
30 % loss    182/200 delivered, 103 duplicates        FAIL (60 s cap)
50 % loss    384/500 delivered, 130 duplicates        FAIL
```

What is proven: framing, sequence/ack bitmap encoding, dedupe by event id.
What is not: retransmission scheduling. Every event is retransmitted (100 %) because the 40 ms
retry timer is shorter than the jitter, and 18 events still never arrive inside 60 s — so the
retry/ack loop needs redesign before this pattern goes into C++. The obvious next step is a
sender-side timer independent of the packet pump, and acking duplicates explicitly.

**Do not port this layer to C++ until the lossy case passes.** That is the whole point of having a
reference implementation first.
