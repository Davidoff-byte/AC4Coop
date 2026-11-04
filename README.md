# AC4 Black Flag co-op

A native C++ plugin that adds two-player co-op to **Assassin's Creed IV Black Flag** (2013,
AnvilNext, PC 1.05). Loaded through a `dinput8.dll` ASI loader; everything is offsets and detours
against `AC4BFSP.exe` (45,056,040 B, sha256 `bc11ad79…ab5150`). No engine source.

**Start with [`README-REVIEW.md`](README-REVIEW.md)** — status, architecture, what is proven vs
unproven, the known-wrong list, and where a reviewer should look first.

## What works

| | |
|---|---|
| transport | UDP peer-to-peer, verified live both directions over a VPN at ~200 ms |
| player puppet | the other player's body, render-only position mirror, collision off, ~33 kB/s |
| kills | host→guest proven live; guest→host built, unverified |
| enemy scope | classified by `entity+0x9C` kind/role, taken from the game's own enums |

## What does not

- guard perception of the other player (the ghost has no perception registration)
- spawning an enemy the guest lacks
- ship sync beyond identity
- the second game build: the mod targets one exe; a different build needs its offsets relocated

## Build

```
MSBuild AC4Coop.sln /nologo /v:m /p:Configuration=Release /p:Platform=x86 /p:PlatformToolset=v143
```

Reproducible: `/Brepro`, no PDB in Release. Current release `33F8FDB08190`, 303,616 B.

Deploy next to `AC4BFSP.exe`: `AC4Coop.asi`, `AC4Coop.ini`, `dinput8.dll`, `known_builds.txt`.

## Documentation

```
docs/ops/      plans, decisions, claims ledger (C1–C25), journals, run sheets, tooling
docs/evidence/ PS3 debug-ELF extracts: enums, class index, name→address map, struct dumps
docs/logs/     real two-machine logs used as evidence
docs/kits/     release manifests
```

Reading order for a newcomer: `docs/ops/JOURNAL-2026-10-30.md` →
`docs/ops/JOURNAL-2026-11-04.md` → `docs/ops/CLAIMS.md` → `docs/ops/PLAN-PS3.md`.

## Rules this project runs on

1. No guessing. Every claim carries a `file:line` or a binary address, or is labelled *unproven*.
2. A claims ledger with an independent check for each entry.
3. One writer per file; one build from a clean tree.
4. Reports are files, not messages — a polling watcher silently dropped three finished reports.
5. Single-player, offline only. No anti-cheat, no DRM work, no piracy.

## Prerequisites — what is NOT in this repo

| needed | why | where from |
|---|---|---|
| **AC4 Black Flag PC 1.05** — `AC4BFSP.exe` 45,056,040 B, sha256 `bc11ad79…ab5150` | every offset targets this exact binary | you must own the game |
| **`dinput8.dll`** — an ASI loader | loads `AC4Coop.asi` into the process | not shipped here; any ASI loader works |
| **`known_builds.txt`** | the build gate refuses to patch a binary it does not recognise | included in this repo |
| **PS3 debug build `scimitar_final.elf`** (0.52 GB, DWARF) | only for the reverse-engineering work; not needed to build or run | you must provide it — see `docs/ops/PS3-SYMBOLS.md` |
| VS 2022 Build Tools, x86 toolset **v143** | the build line above | Microsoft |

A fresh clone builds and produces an ASI: **305,152 B**.

The *size* is stable; the **hash depends on the checkout path** — two clones in different folders give
different digests, because absolute source paths still end up in the image despite `/Brepro` and no
PDB. So "same hash" here means "same build from the same directory", not universally reproducible.

That is also **ahead of what is deployed** (`33F8FDB08190`, 303,616 B) by the multi-instance
diagnostic in `dllmain.cpp` — "builds" and "is the tested binary" are two different things here.

## Path configuration — `tools/paths.py`

No tool contains an absolute path any more. Everything resolves through `tools/paths.py`:

| variable | meaning | required |
|---|---|---|
| `AC4_GAME_DIR` | game install, the folder holding `AC4BFSP.exe` | yes |
| `AC4_KNOWN_EXE` | the build the offsets were derived from | yes |
| `AC4_TARGET_EXE` | a different build being relocated onto | no |
| `AC4_PS3_ELF` | PS3 debug executable with DWARF (`scimitar_final.elf`) | only for RE tools |
| `AC4_SYMBOLS` | folder with `functions.jsonl` / `functions_v2.jsonl` | only for RE tools |
| `AC4_WORK` | scratch root (IDA databases, dumps) | default `./work` |
| `AC4_EVIDENCE` | where tools write output | default `./docs/evidence` |

Check what it resolved:

```powershell
py tools\paths.py
```

Each tool prints usage and exits 2 rather than crashing when a required path is missing.

## What is unfinished, stated plainly

- guard perception of the other player — `docs/ops/AI-T5-G10.md`
- spawning an enemy the guest does not have — `docs/ops/PS3-SYMBOLS.md` §6
- ship sync beyond identity
- the second-build port — `docs/ops/JOURNAL-2026-11-04.md` §11: 22 of 101 sites resolved
- no two-machine run since the last five commits, so everything after `5BC501A06101` is
  compile-and-reason only
