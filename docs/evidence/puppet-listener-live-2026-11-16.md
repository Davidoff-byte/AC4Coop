# Does the replica carry an EventListenerComponent? (live, 2026-11-16)

Build `027f4bd` tree=dirty, asi `7ad22290...CB32F0`, pid 7592, solo replay against peer9 (proto 16) over loopback.
Flags: `[Coop] PuppetListener=1` (report only, Attach=0), `EventProbe=1`. Verdict: **the listener IS present - M1 refuted.**

## Setup notes, both worth keeping

- `BindIP=auto` binds the game to the LAN adapter, so a loopback peer reaches nothing. `BindIP=127.0.0.1` is required.
- `[Net] Mode=host` drops every game datagram until a lobby `AC4L` handshake arrives (`net.cpp:2125`), which peer9
  does not send. `Mode=manual` makes `g_peerReady` 1 from the start (`net.cpp:2622`) and the link comes up.
- `PeerPort` must be the peer's port (`47802`), not the symmetric `47801` - peer9 binds 47802 and the game must
  send there, otherwise it receives nothing.
- The game pauses the world when unfocused; with the loop stalled nothing sends (`CaptureLocalState` in
  `phase1.cpp:13712` never runs). A single `Esc` after focusing resumed it (`[HB]` resumed).
- The puppet does not spawn until the peer announces the game's own `worldId` (`--world 0x32E21E58` here). It is not
  automatic from the peer's side before the first accepted packet.

## Result

```
[Sight] on=1 host=1 puppet=39CEEC10 | ... kind=1 ... puppetPicks=10 ...
[PL] pup=39CEEC10 listener=has ptr=FC609D38 life=-15536 d4=80
```

- `listener=has`, `ptr` non-null: the dispatch predicate (`0x5220E0`) succeeds on the replica, so the
  `EventListenerComponent` is not the stop. The 2026-11-15 drop is **not** explained by a missing listener.
- `+0xD4 = 0x80`: neither of the two handler early-out bits is set (bit 0 gates the whole handler, bit 3 gates the
  apply), so those are not obviously the stop either.
- `kind=1` at `+0x9C`, i.e. the player branch of `0x173E994` should be taken.

## Open, and suspicious

- `life = -15536` (`0xC3B0`) is not a plausible life value; `+0x5C` may not be the replica's life field, or the
  object behind `+0x5C` is not a health object.
- `[Sync] bind sent=0 bound=0` - nothing bound, so the replica is not position-mapped in this run. Guard contact
  geometry is therefore untested here; this run proves the component gate only.

## Next

Re-run with a guard actually striking the puppet and `EventProbe=1`: the `[Evt]` posts already proved to arrive, so
what is needed now is whether the handler chain runs (`0x1C08EE0` reached with the puppet) and what `+0x5C`/`+0xD4`
read at that moment. The `+0x5C` question comes first, since every branch downstream reads it.

Two-machine status: **none**. Build is solo-tested only.
