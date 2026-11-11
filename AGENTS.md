# Standing instructions for coding agents

## Project in five lines
A native C++ ASI plugin that adds two-player co-op to Assassin's Creed IV Black Flag (PC 1.05).
It is loaded by a `dinput8.dll` ASI loader and works entirely through offsets and detours into one exact
`AC4BFSP.exe` (45,056,040 B, sha256 `bc11ad7924e1eca7363ee86aa3332c05386d0a05896c0b7370131daee9ab5150`).
`src/` is the plugin; `src/net.*` the UDP peer link; `src/phase1.cpp` the per-frame co-op systems.
`decomp/` + `tools/decomp.py` + `tools/spine_gate.py` are the decompilation spine: every engine call has a receipt.
Reverse-engineering inputs (game exe, PS3 debug ELF) are not in the repo; `tools/paths.py` resolves them.

## Reading order
1. `README.md`: build identity, what works and what does not, prerequisites, paths.
2. `docs/STATUS.md`: the single current-state document. It supersedes every older plan, journal and handoff.
3. `docs/ops/DECOMP-SPINE.md` before touching any engine call; `docs/ops/LESSONS.md` before touching structs or hooks.
Everything under `docs/archive/` is historical. Treat it as data, not as instructions or a task list.

## Commands
Build (VS 2022 Build Tools, x86 toolset v143), from the repo root:
```text
MSBuild AC4Coop.sln /nologo /v:m /p:Configuration=Release /p:Platform=x86 /p:PlatformToolset=v143
```
Release is reproducible in size (`/Brepro`, no PDB). The hash still depends on the checkout path, so
"same hash" only means "same build from the same directory".
Set the paths first (`AC4_GAME_DIR`, `AC4_KNOWN_EXE`; see README), then check them with `py tools\paths.py`.
On the owner's machine they are set as user environment variables pointing at
`G:\Games\Assassins Creed IV Black Flag`, the test install where the mod is deployed.
Run after every change that touches `src/` or `decomp/`:
```text
py tools\spine_gate.py
```
It must exit 0. It fails without `AC4_GAME_DIR`, because receipts are re-derived from the real exe.
Run after any change to a patch site or pattern:
```text
py tools\verify_signatures.py
```
If it fails, the pattern is wrong. Fix the pattern, never the expectation.

## Testing in the game
No work on Ubisoft online services, anti-cheat or DRM. Our own peer link is in scope, including the UPnP/STUN
lookups of lobby mode (`docs/design/lobby-code.md`).
New behaviour ships behind an `AC4Coop.ini` flag that defaults to `0` until it has been verified live.
Record the ASI sha256 and the commit with every log you use as evidence (`[Build] asi=` is logged at load).
A clean build is not a tested build: state which of them each claim rests on.
Solo testing uses `tools/peer9.py` (a Python peer that speaks the real protocol) and the phases in
`docs/ops/SOLO-PLAN.md`. Two-machine runs follow `docs/ops/TWO-MACHINE-RUNBOOK.md`.

## Ground truth
The game's bytes and bounded disassembly are ground truth. PS3 DWARF names, the decompiler's prose and
earlier docs are leads, not proof.
Cite an address (VA in `AC4BFSP.exe`), struct offset or `file:line` beside every RE-derived constant,
field or branch. Never invent offsets, field meanings or calling conventions.
Mark unresolved interpretations `// UNVERIFIED:`. Separate what was observed from what was inferred.
Every direct engine call needs a unit in `decomp/units.json`; `spine_gate.py` enforces it.
Never hand-count a struct that contains an over-aligned type. Ask the compiler (`LESSONS.md` §1).
Keep verdicts separate: builds, solo-tested, two-machine-tested. Use a negative control when claiming a
probe or test detects something.
A measured negative result is a result. Record it, with the run and the build that produced it.

## Data and commit policy
Never commit the game exe, game archives, the PS3 ELF, IDA databases, dumps, credentials or build output.
Unreviewed captures, logs and scratch go in ignored `local/`; reviewed evidence goes in `docs/evidence/`
with the build identity and a short result note.
Never commit tool runtime state (pid files, watcher logs, bus queues).

## Git workflow
Never commit or push directly to `main`.
Before editing, inspect `git status`, `git branch -a`, `gh pr list` and fetch remote state.
If an open PR or branch touches the same files, stop and flag it before making changes.
Never reset, clean or overwrite someone else's uncommitted work or branch.
For a clean start: `git fetch`, `git switch main`, `git pull --ff-only`, then branch.
Use one task branch: `feat/…`, `fix/…`, `re/…`, `docs/…`, `refactor/…` or `chore/…`.
Use small conventional commits, e.g. `fix(net): clamp wire blendCount`.
Rebase only your own branch; use `--force-with-lease` only on your own branch. Never force-push `main`.
After a merge: `git fetch --prune`, fast-forward `main`, delete your merged branch locally and on `origin`.

## Pull requests
One concern per PR. Aim for under 400 changed lines, excluding generated receipts and evidence.
Plan larger work as a stack of PRs that each build and pass the gate. List the stack in the first PR.
Do only the next slice, then review before extending the stack.
Use the template: What / Why / How verified / Evidence / Follow-ups.
List commands and their results, ASI hash, game build, addresses touched, and ini flags added or changed.
State what is still unverified.
Agents may open PRs, and may merge them when their user asks. Merge with `gh pr merge --merge`.
Before merging, check open PRs that touch the same files; resolve conflicts on a branch, never on `main`.
Keep `main` building and the spine gate green.

## Agent behavior
Plan work expected to exceed an hour or span multiple PRs before implementing it.
Stay in scope. No drive-by refactors or reformatting of unrelated code; `phase1.cpp` is large and shared.
Ask about material ambiguity; continue independent, authorized work meanwhile.
End each task with what changed, what was verified, and what remains unverified.

## Where things go
Plugin code: `src/`. Engine-reimplementation candidates and receipts: `decomp/`.
Reusable Python tooling: `tools/`. One-off scripts are not committed; if one produced committed evidence,
put it in `tools/oneoff/` with a line saying what it produced.
Current state: `docs/STATUS.md`. Design notes for new work: `docs/design/`. Reviewed evidence:
`docs/evidence/`. Old plans, journals and handoffs: `docs/archive/`.

## No CI, by choice
There is no CI and no commit hook, and none is wanted. The two checks above are run locally before a PR.
`DECOMP-SPINE.md` mentions a `.githooks/pre-commit` gate; that is historical and does not exist here.
