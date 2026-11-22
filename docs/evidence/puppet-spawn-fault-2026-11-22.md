# Puppet spawn fault: the first BuildEntityWithSelector faults (2026-11-22)

Handover step 3 (`ops/HANDOVER-MP-BLUEPRINT-2026-11-21.md` §3.4, defect D4 in `combat-timing-2026-11-17.md`).
AC4BFSP.exe md5 `2058342866688f780c8b34526a65bc35`. IDA jobs `D:\ac4ida\cc\jobs\p1`-`p4`.

## 1. The measured fact (logs of 2026-11-16, solo replay rig)
- `[Phase1] EXCEPTION during resolve/build (code=0xC0000005)` on the first spawn attempt in 4 of 7 runs: 4220, 26516,
  28944, 20696. Clean: 32644, 568, 30324. Also 5 times on the guest after reloads in run 3 of `combat-timing`.
- The fault is between `dupSelector=` and `BuildEntityWithSelector rc=`, so it is inside the build.
- After the fault: in 4220 and 26516 the retry 2 s later succeeded; in 28944 and 20696 the game died seconds later
  (WER: an engine worker at `0x004D98F9 lock xadd [ecx],edx`, ecx = 4 / 0xD6, via `0x004DD310` <- `0x0057E212` /
  `0x0057E276`; no AC4Coop frame on the stack).

## 2. Stale builder: REFUTED (same logs)
Every attempt logs the builder handle and object. Faulting and clean runs have:
- the same builder vtable `0x01E57790`;
- the same handle words (`[1]=2C`, `[2]=80000001`);
- the same resource id `00000002`C0A3FCE0`.
In 4220 and 26516 the **same builder object and the same selector** faulted on try 1 and succeeded on try 2. The fault
depends on timing, not on a released builder.

## 3. Static (decompiled)
- `BuildEntityWithSelector` = `sub_5FAC00(builder, Entity** out, selector)` (thiscall). Its only reference is the
  EntityBuilder vtable entry at `0x01E577D4`.
  1. `sub_7A22F0(sel)` and `sub_79DD50(sel)`: prepare the selector against the global row registry (`word_2AC37B2`,
     `dword_2AC37A4`).
  2. `sub_A2D5A0(0, 1)` at `0x005FAC1C`: **opens its own duplication scope**.
  3. `*out = builder->vt[+76](sel)` (create), then `builder->vt[+64](out, sel)` (fill).
  4. `sub_A2F320()`: closes the scope.
  5. `sub_8B2B10(sub_162BEE0(sel))`, then `sub_7A2380(sel)` (releases the selector's buffer).
- The duplication scope is a **per-thread stack**:
  - `TlsIndex` is the dword at `0x04DDF92C` (`0x00A2D5A3: A1 2C F9 DD 04`); the frame array is at `TLS[TlsIndex]+0x18`;
  - the depth dword is at `+2016`;
  - frames are 252 bytes; `sub_A2D5A0` increments the depth and `sub_A2F320` decrements it.
- **Consequence (static, UNVERIFIED live):** our `__except` in `Phase1::TrySpawnPuppet` unwinds out of step 3 and skips
  step 4. The thread's duplication stack stays one frame deep, with a partly built entity in `*out` and the selector
  never released (step 5 skipped). Every later duplication on the game thread then runs one frame too deep.
  - UNVERIFIED that this causes the later worker crash.
- The mod's own scope around `RowSelector::Duplicate` (`DuplicationManager::Begin(1,0)` / `End`) is balanced by
  `__finally`, and is **not** the cause: the build opens and closes its own scope.

## 4. Change (built, not yet run)
On `feat/integration-1121`, `src/phase1.cpp`, `TrySpawnPuppet`:
- **Diagnostics (always on, logging only):** the fault line now carries the exception address, the read/write flag, the
  target address, esp, and the duplication depth before the build and after the fault. A second line lists the first 10
  exe-image return-address candidates on the faulting stack.
- **`[Coop] PuppetBuildRecover` (default 0):** after a fault, call `sub_A2F320` until the depth is back to its value
  before the build (at most 4).

## 5. Pre-registered checks (written before the run)
Rig `spawn_stress.ps1`: 6 pair starts (2 builds each), hold 75 s after each start; cycles 1-3 recovery off, 4-6 on.
- **D1 (diagnosis):** at least one fault line with an address. Report the faulting function and the depth before / after.
- **D2 (leak hypothesis):** on every fault with recovery off, `after = before + 1`. Any other value kills the hypothesis.
- **R1 (recovery, only if D2 holds):** with recovery on, the depth after the pop equals the depth before the build; the
  retry builds a puppet; no copy dies within the hold. With recovery off, report how many copies die.
- A sample with no fault in any cycle is INCONCLUSIVE, not a pass.
