# version.dll proxy: AC4 multi-instance, always on

A standalone `version.dll` for the game folder. It removes AC4's single-instance guard on **every** start: plain
Steam or Ubisoft starts, starts without `AC4Coop.asi` or the ASI loader, and any `AC4Coop.ini` setting. The
plugin's own `HookMultiInstance` path only runs after BuildGate, the launcher arm and two ini keys. The two can be
used together (see below).

## What it does
- `AC4BFSP.exe` (sha256 `bc11ad79…`) imports `VERSION.dll` statically: `GetFileVersionInfoA`,
  `GetFileVersionInfoSizeA`, `VerQueryValueA` (pefile). `version.dll` is not in KnownDLLs on this machine, so the
  copy in the game folder loads before the exe's entry point.
- `DllMain` finds the main module's `KERNEL32!CreateMutexW` IAT slot by name and swaps it. A mutex name that starts
  with `scimitar_mutex_` gets a `_mi<pid>` suffix. Any other name passes through unchanged.
- The guard, verified in IDA (`docs/evidence/single-instance-guard-2026-11-15.md`): `_WinMain@16` 0x00406D25
  `CreateMutexW(NULL, FALSE, L"scimitar_mutex_ac4bf_61979")`. WinMain then waits to own the mutex (0, then 5 s) and
  returns 0 if it cannot. With a unique name per process, every copy owns its own mutex.
- All 17 exports of `SysWOW64\version.dll` are present, with the same names and ordinals:
  - 14 are typed wrappers (signatures come from the SDK via `decltype`) over the real DLL, which is loaded on first
    call and never under the loader lock;
  - `VerLanguageNameA/W` forward to KERNEL32, as in the real DLL;
  - `GetFileVersionInfoByHandle` forwards to `api-ms-win-core-version-private-l1-1-0`. The real DLL on Windows 11
    26200 is a bare `jmp` to that export (disassembled).
- Log: `AC4MultiInstance.log` next to the DLL, a few lines per process start (`slot_ok`, `hooked`, the rename).

## Build
`proxy\version\build.cmd` → `proxy\version\out\version.dll` (x86, static CRT, imports only KERNEL32) and
`out\mi_test.exe`.

## Install / remove
Copy `out\version.dll` into the folder that holds `AC4BFSP.exe`. To remove it, delete it. The exe is not touched,
so the BuildGate pin and Steam file verification are unaffected.

## Verified (offline, 2026-11-15, version.dll sha256 3f357d02…)
- Export table compared with `SysWOW64\version.dll` (pefile): 17/17 names and ordinals match.
- `mi_test.exe` reproduces the guard's ownership check:
  - **Negative control** (no proxy), second copy: `wait=258`, blocked.
  - **With the proxy**, second copy: `wait=0`, owned.
  - Log: `slot_ok=1 hooked=1`, `scimitar_mutex_mitest` → `scimitar_mutex_mitest_mi<pid>`.
  - `GetFileVersionInfoSizeA/GetFileVersionInfoA/VerQueryValueA` through the proxy give the same kernel32 version as
    the system DLL.

## Not verified yet
- **In the game:** whether `AC4BFSP.exe`'s IAT is already filled when the proxy's `DllMain` runs (`slot_ok=1` in the
  log). The test exe is a different binary.
- **Running with AC4Coop.asi:** the plugin's `InstallMultiInstance` would find the slot holding the proxy rather than
  kernel32 and fall back to its hardcoded RVA. The name would then get both suffixes. Not run.
- **What it does not change:** a second copy on one PC is still stopped by Steam/Ubisoft start-up
  (`SteamAPI_RestartAppIfNecessary` / `UPLAY_Startup` → 2; see the evidence file and DECISIONS D-019). This proxy only
  removes the game's own mutex wait.
