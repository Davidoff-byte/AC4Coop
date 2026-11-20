# ShipMirror port — works, but a hang appeared with it on (2026-11-20)

**Branch:** `feat/shipmirror-port` @ `0324a6c` — `c82a696` cherry-picked onto `feat/sail-memento` (`e7487ee`).
**Build:** ASI 760,832 B sha256 `ecb2f17b…3beba6`, stamp `commit=0324a6c tree=clean`.
**Port:** one conflict, in `DumpConfigOnce()`'s key table, resolved by union; `git show --stat` = 1 file,
+152/−3, the same shape as the source commit. Wire untouched (`netstate.h`/`net.cpp`/`net.h` empty diff),
`NET_PROTO_VER` and `PlayerState` unchanged. No duplicate `g_ghostShip`/`g_ghostObjId`/`ismirror`
definitions. Build and gate green.

---

## 1. What worked — the mirror creates guest-side ships

Guest log with `[Coop] ShipMirror=1`:

```
[ShipMirror] created proxy=3D330CD0 for host=4026548270 cls=2 src=3BB289D0 classMiss=0 pool=3
[ShipMirror] created proxy=3D1672F0 for host=4026548404 cls=1 src=38D1A3B0 classMiss=0 pool=4
[ShipMirror] created proxy=3D061080 for host=4026548468 cls=1 src=38D1A3B0 classMiss=0 pool=5
```

Proxies for host AI ships, correct classes, `classMiss=0`, pool growing. So the identity ceiling described
in `SESSION-2026-11-20.md` §4 is addressable: the guest can hold a local object for a host ship.

Sail still applied `via=local` in that run — expected, because the sail memento carries the **player's**
ship id (`wire=4026549543`) and the mirrored host ids were different ships (`…8270/8404/8468`). The
mirror does not change that path, and it does not break it (`ok` counts kept rising).

## 2. What did not — the guest hung, and the mirror is a suspect

`two_copy.ps1` aborted that run:

```
RUN ABORTED: dump: dump failed err=0x8007012B pid 17212 HUNG (not responding, cpu +0.17s in 3s)
             dump D:\AC4-RE\dumps\hang-17212-20261120-181400.dmp
```

The dump file is **0 bytes** — the dump tool failed, so there is no stack to read. The guest's last lines
were ordinary (`[Net] cap SWITCH …`), no `[Sail] EXCEPTION`, no `resolve/build` fault.

**Differential control, same build, same flags, `ShipMirror=0`:** both processes alive after 75 s,
`resp=True`, guest applied **35** sail states, zero mirror lines, no abort.

## 3. Verdict — a lead, not a finding

- n=1 with the mirror on (hang), n=1 with it off (stable). That is **not** enough to say the mirror causes
  the hang.
- A confounder is already recorded in the project: puppet creation faulted in **4 of 7** solo runs on
  2026-11-17 (`ops/evidence/...`, ledger), and one earlier note attributes engine worker crashes to a
  half-built puppet. Neither run here had `SpawnerIdProbe`/`BoardProbe` on.
- What is **proven** here: the mirror builds proxies with the right classes on this base, and the wire is
  unchanged.

## 4. Action taken

`ShipMirror` is left at **0** in the deployed ini, so the deployed build is inert with respect to the
mirror while the cause is unknown. The branch is not merged into the deployed line.

## 5. The next two runs that would settle it

1. `ShipMirror=1` again, twice, with `SailSync=0`/`SailTestCycle=0` — removes sail traffic as a variable
   and gives n=3 for the hang.
2. If it recurs: same with `ShipMirror=1` but the proxy creation disabled after the first N proxies (a
   harness cap), to see whether the hang tracks the number of proxies or the mirror at all.

Also worth fixing separately: the minidump tool returned `0x8007012B` and wrote 0 bytes, so every hang
in an unattended run is currently undiagnosable. That is its own item.

---

## 6. UPDATE the same day: 2 of 3, and the proxy hypothesis is not supported

Third run, identical flags (`ShipMirror=1`, `SailSync=1`, `SailTestCycle=1`), after adding a guard that
refuses to apply a sail memento to a duplicate (mirror proxy or the ghost):

```
both copies alive after 90 s (host 28816, guest 25916)
guest: mirror=32  sailRx=41  stats ok=40 stale=0 unresolved=0 bad=0 exc=0 local=40 refused=0
```

So the combination is **2 crashes in 3 runs**, not 2 of 2. And critically **`refused=0`**: the guard never
fired, because `FiResolve` still did not hand sail a proxy. The hypothesis in the commit message for
`b05a0b9` ("the guest now resolves host ships to proxies, and the setter faults on duplicates") is
therefore **not supported by this run** — it remains a plausible hazard, but it is not what killed the two
copies.

What is still true:

- the mirror creates proxies with the right classes (`classMiss=0`), and the guest applies sail mementos
  `via=local` alongside it;
- one copy died in 2 of 3 combined runs and 0 of 2 single-feature runs — a lead at n=3, not a cause;
- the guard is kept because "the setter faults on a duplicated `GcLShip`" is independently recorded, but
  it is **not** a fix for this crash.

Next diagnostic, in order: (1) reproduce with mirror on and sail off for a 10-minute window, to see
whether the crash is simply intermittent; (2) if it recurs, use the WOW64 minidump fallback (`b1a6584`)
and read the stack instead of guessing.
