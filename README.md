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
