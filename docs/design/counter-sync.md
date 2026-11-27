# Counter / block sync (S0-S3) — defender-resolved counters

RE + design: `ops/evidence/combat-counter-sync-design-2026-11-27.md`. Code: `src/phase1.cpp` (`[Coop] Counter*` block beside
AttackForward), `AttackForward_Post(.., void** outFde)` in `src/player_damage_probe.cpp`. Requires `[Coop] AttackForward=1`
(guest re-posts the host's warnings/hits natively). The guest's engine decides; the host replays that decision.

## Flags (`[Coop]`, all default 0, read once at load)
| key | side | slice |
|---|---|---|
| `CounterProbe` | both | S0: guest logs each response read from the posted FDE; host logs the guard's Ground path per forwarded attack |
| `CounterSync` | both | S1: guest sends each result (CtrA+CtrB); host logs rx with age / round trip |
| `CounterHostApply` | host | S2: 1 = post a FightDamageResponseEvent to the guard; 2 = control (type 0, phase 1). Forced 0 without CounterSync |
| `CounterGuestVeto`, `CounterVetoMs`=1500 | guest | S3: stop the countered warning's re-posts; drop that guard's HITs for VetoMs |
| `AttackReplay` | — | S4 not built (see "Not done") |

## Wire (NET_PROTO_VER 18; datagram format unchanged)
Guest -> host on the intent lane, channels `IC_ATK_A` + `IC_ATK_B`, **one key** wireId `kCtrWire` = 'AVATARO3' (so the host's
per-guard `ForgetIntents` after 2 s idle cannot strand this key's receive order). `IntentMsg.hostMs` = decision time on the
host clock (`PeerClock::HostUs/1000`, 0 = no estimate). Joined by (seq, guard low 32).
```
CtrA 32 B: u64 guard | u32 seq (AF seq of the event resolved) | u8 phase(f+0x54) type(R+0x20) postedPh b48 b49 b4A b4B rsv | u32 r24 r28 r2C
CtrB 32 B: u32 seq | u32 guardLo | u32 r30 r38 r3C r40(variant) r44 r4C
```
R = FightDamageResponse at fde+0x70. Not carried: R+0x10 (slot 147 path), R+0x50 (type 5: rejected on the host).
Host drops a result as `STALE` when (guard, seq) is not an AF event it published in the last 2 s (replay to a rejoined peer).

## Threads / locks
- Guest reads happen right after `AttackForward_Post` returns (dispatch is synchronous), on the player frame, plain reads in
  their own `__try`; no new engine call on the guest.
- Host: `sub_10EFC30`, `sub_18480F0` (EnterCriticalSection `unk_2AC1FCC`) and `sub_64D8C0` (`dword_2AC1FA0`) are called ONLY
  from `CounterHostApplyFire`, run from `OnFrame` after the `OnFrameInner` `__try` (beside `ShipSpawnReplayFire`), outside
  every `__try`, one per frame, after lock-free validation (guard vt + own handle, puppet handle, path gate, queue age <= 500 ms).
- Nothing built here is ever freed (FDE, response event, the extra puppet-handle reference), as `AttackForward_Post`.

## Oracle (PASS / FAIL from log lines; counts from `[CTR] stats`)
| slice | PASS | negative control |
|---|---|---|
| S0 | counter-only run: >= 1 `[CTR] guest resp guard=.. type=20/21/22` per observed counter-kill | no-input run: 0 lines with type 20/21/22 |
| S1 | `[CTR] host rx` count == guest `tx` count (stats); `age` p95 <= one-way p95 + 33 ms; 0 `STALE` in a steady fight | CounterSync=0 on the guest: host rx = 0 |
| S2 | `[CTR] host apply .. ok=1` or `apply+1f .. ok=1` (528 on the guard's path) for every non-skipped post | CounterHostApply=2: every apply `ok=0`, path unchanged |
| S3 | `[AF] owner HIT vetoed ..` with `life` equal to the value before the counter | no-input run: `hitsVetoed=0` |
| S5 | host `ok528` == guest counter-kill count == host guard deaths (two machines) | counter-only vs no-input runs |

Gate skips are logged `[CTR] host apply .. skipped (reason) fallback=OwnerDamage|log`: counter-kill damage still arrives by
OwnerDamage; block types are log-only.

## Not done
S4 `AttackReplay` (I-302): needs new hooks at 0x14BE1A5 and in `sub_14BDFE0` plus the four uncovered 155/160 callers, a
deferral of the host's own attack call, the slot-160 null-deref on copies (R-1027), and AF off per replayed guard. Not a
minimal slice; left for its own design pass.

## UNVERIFIED (live)
Guest decision reaches `sub_184C9D0` from a forwarded warning; guard -> 528 in time (2 x one-way latency); `sub_184DA90` /
`sub_13F2A60` / `sub_1828140` run with the puppet as partner without faulting; event/FDE pool lifetime; hit-phase block
codes from `sub_18644D0`; visual alignment of the synced pair.
