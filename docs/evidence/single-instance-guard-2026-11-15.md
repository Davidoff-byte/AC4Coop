# AC4's single-instance guard (2026-11-15)

AC4BFSP.exe md5 2058342866688f780c8b34526a65bc35. Prompted by AU-RW ACU-RE-0086, where ACU's guard is a named semaphore
`scimitar_semaphore` that is created but never held.

## The guard (static, IDA + exe bytes)
- **Name:** the wide string `scimitar_mutex_ac4bf_61979` at VA 0x01E3E44C (file offset 0x1A3D24C). The string `scimitar_semaphore`
  is not in the exe (ascii and wide search: 0 hits).
- **The only `CreateMutexW` call** (IAT 0x01E3A20C, all xrefs) is in `_WinMain@16`:
  - `0x00406D1C push offset Name` / `0x00406D25 call CreateMutexW(NULL, FALSE, name)`;
  - `WaitForSingleObject(h, 0)`; `bl` = 1 if ownership was not immediate;
  - loop: `WaitForSingleObject(h, 5000)` (0x1388):
    - 0 (owned): go to 0x00406D6A; if `bl`, `Sleep(1000)`; then start-up continues;
    - `WAIT_TIMEOUT` (0x102) or `WAIT_FAILED`: `CloseHandle`, **`WinMain` returns 0**: the copy exits;
    - anything else (`WAIT_ABANDONED`): retry the wait.
- So the running game **owns** the mutex. A second copy waits about 5 s and exits silently. (Unlike ACU, where the semaphore merely exists.)
- **Not the guard:** `CreateSemaphoreA` (IAT 0x01E3A210) is called only from `InitBufferSynchro` (0x004070CB), `sub_826700`
  (0x00826712) and `sub_BAE900` (0x00BAE912), i.e. buffer synchronisation. Whether those semaphores are named was not checked.
- Start-up also tests the command line for `/launchedfromotherexec` (0x00406D90) and `-offline` (0x00406DE2). This is likely
  the store-relaunch path; not traced.

## Already defeated by the plugin (live)
- `src/dllmain.cpp` `InstallMultiInstance` / `MyCreateMutexW` (HookMultiInstance) IAT-hooks that `CreateMutexW` and appends
  `_ac4coop_<pid>` to the name.
- Live, pid 33660 (read-only `OpenMutexW(SYNCHRONIZE)`): `scimitar_mutex_ac4bf_61979` = **absent** (error 2);
  `scimitar_mutex_ac4bf_61979_ac4coop_33660` = **exists**. Each copy owns its own mutex, so the 5 s wait always succeeds.
- The comment at dllmain.cpp:464 says the game checks `GetLastError() == ERROR_ALREADY_EXISTS`. The real check is the ownership
  wait above. The rename defeats both.

## What still blocks two copies on one PC
The store layer (Steam / Ubisoft Connect), recorded in DECISIONS D-019. AU-RW's ACU result gets past it only with a
**non-Ubisoft** `uplay_r1_loader64.dll` (their ACU-RE-0085/0086); that is DRM replacement and out of scope here.
- Open, legitimate test: with the genuine install, start a second `AC4BFSP.exe` directly from the game folder while one runs,
  and record whether it starts or relaunches through the store and exits.

## Direct second launch with the genuine install (2026-11-15 12:58 / 13:00, owner's OK)
The first copy was pid 29656, started by Steam as `AC4BFSP.exe -uplay_steam_mode` (build 5290bc0, asi 048a2d96).
- **No arguments** (pid 8620): the plugin loaded fully (build check KNOWN, hooks installed, probe pipe opened), then the
  process **exited with code 0 within 2 s**. No window, no relaunch: no new AC4BFSP or store process in 60 s. pid 29656 was
  unaffected.
- **With `-uplay_steam_mode`** (pid 33648), the store's own launch line: same result, exit 0 within 2 s.
- **Not the game's mutex:** 29656 holds only `scimitar_mutex_ac4bf_61979_ac4coop_29656`; the original name is absent. A
  second copy had nothing to wait on, and the mutex path waits 5 s anyway.
- **The two silent code-0 exits on the store layer (IDA, both verified code):**
  - Ubisoft Connect: `sub_507FA0` (only caller `_WinMain@16` 0x00406E04): `UPLAY_Startup(...)` 0 → continue; **2 → return
    false with no message**; any other value → MessageBoxW error.
  - Steam: `sub_497160` (from `sub_497220`): `SteamAPI_RestartAppIfNecessary(242050)` true → `sub_A475E0(0)` =
    `ExitProcess(0)`; then `SteamAPI_Init`.
  - Which of the two fired was not determined; telling them apart means probing the store/DRM layer, which is out of scope.
- **Verdict:** with the genuine install, a second copy on one PC is stopped by the store layer, not by the game. Same-PC
  two-instance stays unavailable within the project rules (DECISIONS D-019 stands); two rendered worlds need a second
  machine (or a second account and licence on a separate OS).
