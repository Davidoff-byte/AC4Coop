# Intention lane (proto 15): merge + first in-game runs, 2026-11-15

`feat/intent-lane` (58a4835, another session's branch) merged into `feat/launcher` as fb1bc59 (fast-forward of a tested
merge commit). One conflict: `net.cpp` SelfTestWire return line, resolved by keeping both checks (okT restart from
feat/launcher, okI intent lane). Build: asi 44bce40c769b289ea32d7e495e9bc1364a943129f638e86a6f856c4bb853ddc7.
Verdict: **builds + offline-tested + solo in-game (idle lane)**. Not two-machine tested; nothing publishes intents yet.

## Offline (merged tree)
- MSBuild clean; `py tools\net_selftest.py` PASS (wire codec incl. restart, intent lane: golden, parse, handshake, deliver,
  ack, dedup, reorder, loss30, rejoin), `wire_tap.py --selftest` PASS (proto 15 conversions, intent datagrams),
  `lobby_peer.py --selftest` PASS (proto 15 golden vector vs net.cpp), spine_gate VERDICT OK, verify_signatures ORACLE PASS.

## In game
| run | ini | result |
|---|---|---|
| A, pid 14276 | IntentLane absent (0) | puppet replay with `wire_tap.py replay ... --as-proto 15`: `[Net] spawn succeeded`, rx 243 pkt, rejects 0, protoMismatch 0, puppet held at (54.2,-74.3,2.9) (`[PupNav]`) |
| B, pid 15804 | IntentLane=1 | debug output (captured with a DBWIN listener; the net layer logs only through OutputDebugString): `[Net] intent lane ON (proto 15, inc=0a92000a)`, in-game wire + intent-lane + lobby selftests PASS; replay: the peer counted `rx_intent=240` datagrams from the game in 60 s (lane handshake/keepalive; wire_tap does not answer them), puppet link unchanged (rejects 0, protoMismatch 0), no REJECTED lines |

Open: run B logged one caught `[Enum] EXCEPTION (code=0xC0000005)` at the first frame (phase1's one-shot entity-list
probe). It did not occur in run A with the same build; timing-dependent, not attributed to the lane (UNVERIFIED).
Not tested: a lane exchange with a peer that answers the handshake (needs a Python or second-game peer; part of the R3 rig).
