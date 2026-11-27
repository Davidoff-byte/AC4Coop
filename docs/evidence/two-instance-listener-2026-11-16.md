# Two-instance pair run: the replica has the listener on both copies (2026-11-16)

Build `027f4bd` tree=dirty, asi `7ad22290...CB32F0`. Cop A pid 7816 (host), cop B pid 33196 (join, slot 2).
Install `D:\SteamLibrary\steamapps\common\Assassin's Creed IV Black Flag`. Flags `[Coop] PuppetListener=1` (report only),
`EventProbe=1`. Verdict: **solo tested on one PC, not two-machine.** M1 refuted on both sides.

## Recipe that works (repeat this, not the peer9 route)

1. Saves + ini backed up. `[Net] BindIP=auto` (the LAN adapter is what the code advertises), `PeerPort=47801`,
   `LocalPort=47801`, `Mode=host`, `JoinCode=` empty, `[Coop] AutoContinue=1`, `[Pilot] Enabled=1`.
2. Start cop A from the exe. Wait for `[Lobby] host LAN code <X> (<lan-ip>:47801)` in `net_log.txt` (~5 s).
3. Switch the ini to `Mode=join JoinCode=<X>` **just before** starting cop B, then restore the ini to `Mode=host`
   once B has read `Net::Init` (`[Net] cfg ... mode=join joinCode=...`). B takes instance slot 2 and binds ephemeral.
4. `[Lobby] guest joined <ip>:<port>` in `net_log.txt` = handshake up.
5. `py -3.14 tools/pilot.py --pid <A> boot --pulses 4 --wait 6`, then the same for B. Both land at ~(76,-74,1.8).
6. Watch `phase1_log_<pid>.txt` for `[Sight] host=1 puppet=...` on A and `host=0 puppet=...` on B.

Prerequisites verified in code, not assumed: the `CreateMutexW` IAT hook that defeats Steam's single-instance guard is
unconditional (`dllmain.cpp:499-511`); `JoinCode` is read from the ini at `net.cpp:2486` and drives `StartJoin`
(`net.cpp:1654`); `version.dll` proxy and `dinput8.dll` loader are present in the install.

## Result

```
A 7816: [PL] pup=3A3BE300 listener=has ptr=FC699440 life=-20208 d4=4C
B 33196: [PL] pup=3A346F60 listener=has ptr=FC609558 life=-3344  d4=84
```

- **Listener present on both replicas.** The component gate is not the stop in either direction.
- `d4` differs per side and over time (0x80 single-copy earlier, 0x4C / 0x84 here). A's replica had **bit 3 set** -
  the bit that makes the apply (`0x115ACC0`) return early - so that bit is at least *sometimes* set on a fresh
  replica. It is not a constant property of being a puppet.
- `life` is not a plausible value on either side (`-20208`, `-3344`). Two independent copies agree, so this is
  systematic, not one bad object: `entity+0x5C` is probably not the replica's life field. Every downstream branch
  reads it, so this is now the first question.

## Why this route and not `peer9`

`peer9.py` needs `Mode=manual`, a loopback `BindIP`, `PeerPort=47802` and an explicit `--world`; it also cannot
parse the proto-16 entity lane. The two-copy route uses the real handshake and needs none of that. `STATE.json`'s
"two game instances NOT AVAILABLE (Steam blocks the second launch)" is **stale** - it was already false on 2026-11-15.

## Not checked

Guard contact on the replica (no fight provoked in this run), appearance on screen, and the same-save-slot question
(both copies write slot 437; the 2026-11-15 receipt already listed that as unchecked).
