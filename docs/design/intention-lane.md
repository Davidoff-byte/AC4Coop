# Intention lane (proto 15)

Status: transport built, offline-tested, **not wired**. Nothing publishes or consumes intentions yet, and no
build with `[Coop] IntentLane=1` has run in the game or between two machines.

Why: `docs/design/unity-netcode-mapping.md` section 2, item 3. Guards should be driven by a few replicated
*intentions* per guard, not by animation parameters. Each intention is one reliable message per change, and the
last message per channel is kept for a peer that joins or rejoins later. This lane is that transport. Two things
belong to the caller and are not decided here: what a channel's payload means, and which engine setter applies
it.

Code: `src/netstate.h` (wire structs, channel ids), `src/net.h` (API), `src/net.cpp` (`IntentLane`,
`SelfTestIntent`).

## Wire format

There is one new datagram type on the game socket, magic `'AC4I'` (`NET_INTENT_MAGIC` 0x49344341). The other
layouts did not change: `PlayerState` (576 B), the entity header (36 B) and its records, and `LobbyPacket`.
`NET_PROTO_VER` went from 14 to 15 so that a proto-14 build is refused at the lobby handshake, as with every
earlier bump. All fields are little-endian and unpadded.

| Part | Bytes | Fields |
|---|---|---|
| header (`IntentWireHeader`) | 28 | `u32 magic, u32 protoVer, u32 senderInc, u32 peerInc, u32 hostMs, u32 ackCum, u8 ackRanges, u8 msgCount, u16 reserved(0)` |
| ack run, x `ackRanges` (max 16) | 6 | `u32 firstMsgId, u16 count` |
| message, x `msgCount` (max 64) | 27 + len | `u32 msgId, u32 slot, u64 wireId, u8 channel, u8 flags, u8 len, u32 chanSeq, u32 hostMs, u8 payload[len]` (len max 32) |

A datagram stays within 1400 B, the same budget as the entity chunks. The receiver rejects the whole datagram,
before applying anything, if any of these hold:

- `senderInc` is 0, or `reserved` is not 0.
- There are more than 16 runs or more than 64 messages, or there are runs while `ackCum` is 0.
- A `msgId` is 0.
- A channel is 0 or 16 or above.
- A flag bit other than `IF_BASE` is set.
- A payload is longer than 32 B.
- The datagram is truncated, or has trailing bytes.

A golden 64-byte datagram is in both `net.cpp` (`kGolden`) and `tools/lobby_peer.py` (`INTENT_GOLDEN_HEX`). The
Python selftest also checks that the two copies are byte-equal.

**Channels** (`IntentChannel`): 1 ACTION, 2 LOCO, 3 WEAPON, 4 LOOKAT. Ids 1 to 15 are legal on the wire, so a
channel can be added without a layout change. The lane delivers channel ids that it does not name.

**Guard key**: `{u32 slot, u64 wireId}`, the same pair the entity lane carries. The identity is `wireId`. When
`wireId` is 0, the identity is `0x8000000000000000 | slot`, the same convention as `ShadowId`. `slot` is carried
as data, and the latest value wins.

## Semantics

- **Incarnation.** Every process picks a random, non-zero `senderInc` at `Net::Init`. `peerInc` is the
  receiver's incarnation as the sender knows it, and is 0 until the sender has heard from the receiver.
  - A datagram whose `peerInc` is not the receiver's own incarnation is answered, but its acks and messages
    are ignored (`ID_FOREIGN`).
  - While the peer's incarnation is unknown, the lane sends no messages. It sends only a header every 250 ms.
- **New or restarted peer.** A datagram arrives with a `senderInc` different from the one stored. The lane
  then resets both directions:
  - receive order state, ack window and held messages;
  - the outbox and the `msgId` space (back to 1).

  Then every persisted value goes into the outbox with `IF_BASE` set. This is the "replay on join/rejoin". A
  peer that comes back with the same process keeps its incarnation, so its outbox simply continues.
- **Reliability.** Each message gets a per-peer `msgId`. The receiver acks with `ackCum` (all ids below it
  arrived) plus up to 16 SACK runs, tracked in a 2048-id window. The sender resends an unacked message after
  100 ms, or after 1000 ms while the peer has been silent for more than 2 s.
  - Acks ride on any intent datagram. If nothing else is due, a header-only datagram carries them: that is
    the "small ack packet". A header-only keepalive also goes out every 1 s.
- **Order and dedup.** Each `(guard, channel)` has its own `chanSeq`. The publisher assigns it, and it
  increases by 1 per publish. The receiver delivers each key strictly in `chanSeq` order:
  - A message below the next expected `chanSeq` is a duplicate and is dropped.
  - A message above it is held, up to 256 held in total, until the gap fills.
  - A `msgId` that already arrived is also dropped.

  Channels and guards are independent: a gap on one key never delays another.
- **`IF_BASE`.** It is set on the first message of a key to a peer, and on every replayed persisted value. A
  receiver starts a key only on a base. It may also re-base a key forward, dropping what was held below the
  base. A consumer should apply a base as a full state, not as a delta.
- **Persistence.** Up to 512 `(guard, channel)` keys are kept (128 guards x 4 channels), each with its last
  value. `Net::ForgetIntents(slot, wireId)` drops a guard: it is no longer replayed, and its receive state is
  cleared. When a key is created again, it starts above every `chanSeq` already issued, so a receiver never
  takes it for an old message.
- **Overflow.** If more than 1024 messages to one peer are unacked (a peer that stopped acking), the backlog is
  dropped and only the persisted values are re-sent as bases. This is counted in `resyncs`. The intermediate
  values are lost by design; the last value per key is not.
- **Resource refusal.** If the receiver has no room for a message (no key slot, no holding slot, a full
  delivery queue), the message is not acked, so the sender sends it again.

## API (`src/net.h`, namespace `Net`)

```cpp
bool IntentLaneOn();
bool PublishIntent(uint32_t slot, uint64_t wireId, uint8_t channel, uint32_t hostMs, const void* payload, uint8_t len);
bool PollIntent(IntentMsg& out);   // next message; per-channel order
void ForgetIntents(uint32_t slot, uint64_t wireId);
void PumpIntents();                // Net::Send and Net::SendEntities already call it
void GetIntentStats(IntentStats& out);
```

- `IntentMsg` holds `slot, wireId, channel, flags (IF_BASE), len, seq, hostMs, payload[32]`.
- `hostMs` is passed through untouched. Stamp it with the same QPC clock as `PlayerState::hostTimeMs`.
- All calls take `g_intentLock`, and are safe from the game thread while the receive thread runs.
- `PumpIntents` runs at the end of `Net::Send`, which phase1 calls only while the local player is captured,
  and `Net::SendEntities`. A consumer that needs the lane outside the world must call it once per frame.

## The flag

`[Coop] IntentLane` defaults to 0. When it is off:

- every call above is a no-op returning false or 0, and nothing is sent;
- a received `'AC4I'` datagram is counted (`ignoredOff`, and the rx byte counters) and dropped. It is not a
  reject.

Either way, a proto-15 peer's player and entity datagrams parse as before. Intent datagrams never refresh
`g_lastRecvMs`: liveness stays defined by the player and entity lanes.

## Tests and gates

Run `py tools\net_selftest.py`. It compiles `net.cpp` with `AC4COOP_NET_SELFTEST_MAIN` into `bin/net_selftest/`
and runs `SelfTestWire`. That covers the entity codec as before, plus `SelfTestIntent`:

- **golden**: the 64-byte datagram;
- **parse**: 11 rejected shapes (wrong proto or magic, the malformed cases above), plus one positive control;
- **handshake**;
- **deliver**: payload, `hostMs`, `IF_BASE`;
- **ack**: no resend before the timeout, a resend after it, and after the ack the outbox drains and nothing is
  re-sent;
- **dedup**: the same datagram twice;
- **reorder**: `chanSeq` n+2 before n+1;
- **loss30**: 30% loss both ways plus 0 to 2 ticks of delay. 200 messages (10 guards x 4 channels x 5
  updates) must each arrive exactly once, in order and intact, with the outbox drained;
- **rejoin**: the guest restarts. A must replay exactly the 40 last values, as bases, and nothing older.

The tests were checked against six mutated lanes, each run on a scratch copy and none committed: no dedup, no
holding of out-of-order messages, no resend, no replay, acks ignored, `IF_BASE` never set. Every mutation made
the selftest fail.

The Python side is covered by two selftests:

- `py tools\lobby_peer.py --selftest`: intent sizes and ids against `netstate.h`, and the golden vector against
  `pack_intent`, `unpack_intent` and `net.cpp`.
- `py tools\wire_tap.py --selftest`: `--as-proto 15` from proto 13 and from proto 14 taps, and intent datagram
  classification. Intent datagrams in a tap are counted but never replayed.

## Not done / unverified

- Not wired into `phase1.cpp`. That is the next slice. It needs each channel's payload layout and the engine
  setter that applies it, which is RE that another session owns. Re-applying the current state must be a no-op
  (`unity-netcode-mapping.md` 2.3, `65DCB0`).
- Never run with the flag on in the game, or between two machines. The timers (100 ms resend, 1 s keepalive)
  are untuned.
- One peer only. `IntentPeer` holds everything a second peer would need, but the lane has one of them, as the
  link does.
- Ownership is per sender: each side's published keys go to the other side. Per-entity ownership (unity
  mapping item 9) would need an owner field. The design leaves room for one; it is not built.
