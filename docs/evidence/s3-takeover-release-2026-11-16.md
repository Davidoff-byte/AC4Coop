# Stage 1 take-over: what exists, the release path, and a heap-corruption crash (2026-11-16 01:00-01:25)

Stage 1 in the take-over form proposed by the AU-RW session: the linked guest guard becomes the host guard's copy.

## Code map (read-only agent, src/phase1.cpp, line numbers at commit d82af41)
- **AI already off on link:** every packet, `ApplyGuards` disables the linked body's BhvGenericNPC (vtable 0x026E34D8) when the
  engine re-enabled it, and clears `CharacterController::m_DriveEntity` (about :8414-8420). The body's Human statechart keeps
  running (comment about :6721).
- **Writes to a linked body:**
  - transform (`SetGlobalMatrixInternal`, physics only on a jump over 5 m);
  - alias copy (`MirrorActions=1`);
  - optional full animation (`GuardAnimFull=0`);
  - health (`DamageRelay=0`);
  - death (always on; the engine kill obeys `GuestKillMirror=1`).
  - No fight or conflict state is written to guards.
- **Release gap:** AI was turned back on in only one place, the wide-bind stale-slot release (about :8517-8528). Every other
  unlink (`GuestObjForget`, repoint, the other slot releases) left the body a statue with its behaviour off.
- **Two health pools:** the guest's own combat damages a linked body locally. Partial damage is never relayed (dropped on
  the guest about :6183-6188, ignored by the host about :10699-10712); only the death crosses (`GuestPushDeath` → `KIND_KILL`).

## Change: commit 4b3e9bc (asi bbeabb58...)
- `TakeoverNote(best)` at the disable site remembers every body taken over (BindLock on).
- `TakeoverSweep()` runs every 5 s on the guest. A body that is still alive but **no longer linked** gets its behaviour
  re-enabled and its drive flag set (the same two writes as the stale-slot release).
- `[Bind] takeover takenOver= returnedToAI= dropped(bodyGone= deadLocally=)` and `[Bind] returned ent=...` lines.

## Runs (two copies, `FightIntents=1` only; radius 12 and lock from the defaults)
1. **32460 (guest) / 26008 (host): guest crashed** before the release step. Its last applied host position was still the
   soldier area, so no unlink had happened and no `[Bind] returned` line was logged. Details below.
2. **18604 (guest) / 5632 (host): PASS.**
   - Together: `takenOver=7 returnedToAI=2`.
   - Host teleported about 250 m away: `takenOver=6 returnedToAI=7`, with five `[Bind] returned ent=... to local AI (no longer
     linked)` lines and one `[WideBind] released`.
   - `[Bind] lock=1 linked=6 doubleNow=0 ... repointed=0 blocked(0/0)`. Both copies alive, no Application-log crash entry.
   - The 6 links that stayed: the host scopes sync by distance to the **guest** and still had those soldiers loaded at
     250 m, so they remain host guards.

## The crash (run 1)
- **Application log:** `AC4BFSP.exe ... Exception code: 0x80000003 ... Fault offset: 0x004139d2`, pid 0x7ECC = 32460, 01:12:53.
- **Full dump:** written by Windows to `%LOCALAPPDATA%\CrashDumps`, moved to `D:\AC4-RE\dumps\AC4BFSP.exe.32460-20261116.dmp`
  (98.6 MB, not committed).
- **The site** (ida_batch, md5 2058342866688f780c8b34526a65bc35): `0x008139D2 int 3` in `sub_813930`, a small-bin
  allocator (dlmalloc shape: size classes `(n+0x13)&~0xF`, bin map at `[edi+34h]`, bins at `[edi+ecx*8+58h]`). The int 3 is
  the branch `0x008139C5 cmp esi,[edi+44h]; jb 0x008139D2`: a free-list pointer below the heap's lowest address. That is a
  **heap-corruption assert**; the corrupting write happened earlier, elsewhere.
- **Not established:** whose write corrupted the heap (no stack: no debugger is installed to read the dump). Run 2, the same
  build and setup, did not crash.
- **Also logged:** at 01:05:45, guest 14988 of the previous test hit `0xc0000005` at address 0 (a null call). That is
  the moment the test pair was being closed, so it may be a shutdown crash. Dump `D:\AC4-RE\dumps\AC4BFSP.exe.14988-20261116.dmp`.
- **Correction to earlier "silent exit" notes:** Windows Error Reporting does write local dumps for these crashes. Earlier
  checks filtered the Application log on the message text and found none at those times; the S0 exits may have been
  different (not re-checked).

## Pass/fail (reply's list)
| check | result |
|---|---|
| `[Bind]` linked / locked / dropped with reason / taken over | reported |
| zero guards linked twice | PASS (`doubleNow=0`) |
| an unlinked body returns to its own AI | PASS in run 2 (`returnedToAI=7`) |
| guest attack on a linked guard: one death, decided by the host | not done (S9) |
| no crash | **FAIL in 1 of 2 runs**: heap-corruption assert, cause unknown |
