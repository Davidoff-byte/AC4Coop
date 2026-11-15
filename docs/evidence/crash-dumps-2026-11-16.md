# Two crash dumps read with cdb (2026-11-16)

- **Tool:** cdb 10.0.26100.7705 (Windows SDK "Debugging Tools", installed today from the SDK bootstrapper in the package cache,
  Microsoft-signed). x86 cdb, no symbol server (`-y C:\nosym`): frames show `AC4BFSP!<nearest export>+offset`, and the VAs
  below are the absolute addresses.
- **Dumps:** Windows Error Reporting local dumps, moved to `D:\AC4-RE\dumps\` (not committed).
- **Build in both:** asi c0be5c6b (14988) / bbeabb58 (32460), D: Steam install, pinned exe. Context:
  `s3-takeover-release-2026-11-16.md`.

## 1. Guest 32460: `0x80000003` at `0x008139D2` (01:12:53, during a take-over test)
- Exception context (`.ecxr`): `eip=008139d2 int 3`, in `sub_813930`, the small-bin allocator. The int 3 is its
  corruption check (`0x008139C5 cmp esi,[edi+44h]; jb`).
- Thread 10, a worker thread (`BaseThreadInitThunk` → `0x00F1DE71` job loop). Stack, inner to outer:
  - `0x0080A0D4` / `0x00406228`: allocation of 0x20 bytes;
  - `0x00A08CAF`, `0x00A09AD9`, `0x0079AF11`, `0x007340FC`, `0x004D0063`, `0x004C716B`, `0x00651D73`, `0x00658CFD`, `0x0065DDAC`;
  - **`0x006625F9`**: return address inside `SetTargetAction` (0x006625D0);
  - `0x01D13C43` ... `0x01D809A5`: HumanGround FSM code (0x01Dxxxxx band, as the S2 slots);
  - **`0x0052C249` / `0x0052DF87`**: the AIActor context update (S2: `sub_52DF80` is the native context state machine);
  - `0x005500DC`, `0x00A327A6`, `0x00A1DB19`, `0x008165CE`: job dispatch.
- **Reading:** a Human's own state machine, updating on a worker thread, switched action (`SetTargetAction`). That
  allocation found the heap already corrupted. The dump shows where the damage was **detected**, not who wrote it.

## 2. Guest 14988: `0xC0000005`, eip = 0 (01:05:45, while the test pair was being closed)
- Exception context: `eip=00000000`, `ecx=3db58930`: a virtual call through a null slot.
- Worker thread. Stack: `0x006781FB` (caller of the null call) ← `0x0067DAD7` ← `0x0067DC6B` ← `0x0073650D` ← `0x0067DC0A` ←
  `0x004C9161` / `0x004CAC5E` / `0x004CB443` ← **`0x00662648`** (action-system update, next to SetTargetAction) ← job
  dispatch.
- The same band as the existing guards: `sub_679220` (`[Coop] GuardNullActionTarget`) and the deref guard sites
  `0x006795FD` / `0x006817FA` (`AC4Coop_deref_guard_*.log`). This run logged
  `[AG] disabled by GuardNullActionTarget=0 (resolved, not installed)`.
- **Reading:** the action system called through a stale or freed target. Whether shutdown alone causes this was not
  checked.

## 3. Guest 22296 (2026-11-15 21:26, frame-cap run 3, "exited during the moves"): the same family as 2, mid-run
- The dump was still in `%LOCALAPPDATA%\CrashDumps\AC4BFSP.exe.22296.dmp`. That S0 "silent exit" was a crash.
- `eip=00000000`, `ecx=36b32f20`, worker thread. Stack: `0x0067DC0A` ← `0x0073650D` ← `0x004C9161` / `0x004CAC5E` /
  `0x004CB443` ← **`0x00662648`** ← job dispatch. This is the same chain as 14988, and it happened during play, not at
  shutdown.
- Its last phase1 log line: `[Net] action -> 1612324425 ch=0 ly=0 ratio=0.00 blendIn=0.20 (puppet, cur was 1612324400)`,
  our own `SetTargetAction` write on the puppet from the player's update (replica off, actions mode).
- `[Coop] PuppetActionsOwnUpdate` (default 0) exists for exactly this. Its code comment (review 2026-11-14) says the puppet
  writes "can overlap the puppet's own update on another worker thread". No evidence file shows it measured live.
- No dumps exist for the boot-time exits (10104, 27008, 29188). Those remain unexplained.

## What the dumps point at (hypothesis, UNVERIFIED)
- Both crashes are in the engine's **action system on a worker thread**.
- The code map (`s3-takeover-release-2026-11-16.md`) notes two writers on a linked guard's action stack:
  - its own Human statechart keeps running (the behaviour component is off, the statechart is not);
  - our mirror writes the host alias with `SetTargetAction` from the frame path (`MirrorActions=1`).
- Cross-thread writes into one ActionComponent are a plausible source of both the heap corruption and the stale target.
- **Existing levers, all default 0:**
  - `[Coop] MirrorActionsDeferred`: queue the alias write to the body's own update, the same thread as its statechart;
  - `[Coop] PuppetActionsOwnUpdate`: the same for the puppet;
  - `[Coop] GuardNullActionTarget`: the null-target guard.
- **The test that would decide it:** N runs of the take-over test with those flags on against N with them off, counting
  crashes. Not run.

## Correction to earlier notes
The "silent exits" were checked by filtering the Application log for "AC4BFSP" at the time and nothing was found. WER
does write local dumps, so the earlier exits (S0 runs, 2026-11-15) were not necessarily silent. Not re-checked:
`%LOCALAPPDATA%\CrashDumps` was cleaned of today's two only.
