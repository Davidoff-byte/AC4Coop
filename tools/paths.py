"""Single source of truth for every filesystem path the tooling needs.

Nothing else in `tools/` should contain an absolute path. Override any of these with an
environment variable.

    AC4_GAME_DIR    game install, the folder containing AC4BFSP.exe            (required)
    AC4_KNOWN_EXE   the build the plugin's offsets were derived from         (required)
    AC4_TARGET_EXE  a different build being relocated onto                   (optional)
    AC4_PS3_ELF     PS3 debug executable carrying DWARF                      (optional)
    AC4_SYMBOLS     folder with functions.jsonl / functions_v2.jsonl         (optional)
    AC4_WORK        scratch + analysis root (IDA databases, dumps)           (default ./work)
    AC4_EVIDENCE    where tools write their output                           (default ./docs/evidence)

Required paths raise at import with a message naming the env var to set. Optional ones resolve to
None and only raise when something actually uses them (`require_ps3()`, `require_symbols()`), so a
tool that does not need a 0.5 GB ELF still runs on a machine without one.
"""
from __future__ import annotations

import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))          # .../AC4Coop
SRC = os.path.join(REPO, "src")
TOOLS = os.path.join(REPO, "tools")
DOCS = os.path.join(REPO, "docs")

GAME_CANDIDATES = [
    r"D:\SteamLibrary\steamapps\common\Assassin's Creed IV Black Flag",
    r"C:\Program Files (x86)\Steam\steamapps\common\Assassin's Creed IV Black Flag",
    r"C:\Program Files (x86)\Ubisoft\Ubisoft Game Launcher\games\Assassin's Creed IV Black Flag",
    r"D:\Program Files (x86)\Ubisoft\Ubisoft Game Launcher\games\Assassin's Creed IV Black Flag",
    r"D:\Games\Assassins's Creed 4 - Black Flag",
]


def _env(*names):
    for n in names:
        v = os.environ.get(n)
        if v:
            return v
    return None


def _required(label, envs, candidates):
    v = _env(*envs)
    if v and os.path.exists(v):
        return v
    for c in candidates:
        if c and os.path.exists(c):
            return c
    raise SystemExit(
        f"[paths] cannot find {label}.\n"
        f"        set {envs[0]}\n"
        f"        looked at:\n" + "\n".join(f"          {c}" for c in candidates if c)
    )


def _optional(envs, candidates):
    v = _env(*envs)
    if v and os.path.exists(v):
        return v
    for c in candidates:
        if c and os.path.exists(c):
            return c
    return None


GAME_DIR = _required("the game install (folder containing AC4BFSP.exe)", ["AC4_GAME_DIR"],
                     GAME_CANDIDATES)

KNOWN_EXE = _required("the reference AC4BFSP.exe", ["AC4_KNOWN_EXE"],
                      [os.path.join(_env("AC4_GAME_DIR") or "", "AC4BFSP.exe")] +
                      [os.path.join(g, "AC4BFSP.exe") for g in GAME_CANDIDATES])

TARGET_EXE = _env("AC4_TARGET_EXE") or KNOWN_EXE

PS3_ELF = _optional(["AC4_PS3_ELF"], [
    r"D:\Games\acbf_ps3_gold\scimitar_final.elf",
    os.path.join(REPO, "..", "scrimitar_final.elf"),
    os.path.join(REPO, "..", "..", "..", "acbf_ps3_gold", "scimitar_final.elf"),
])

SYMBOLS = _optional(["AC4_SYMBOLS"], [
    os.path.join(REPO, "..", "..", "RE-AC4", "symbols"),
    os.path.join(REPO, "..", "RE-AC4", "symbols"),
    os.path.join(REPO, "symbols"),
])

WORK = _env("AC4_WORK") or os.path.join(REPO, "work")
EVIDENCE = _env("AC4_EVIDENCE") or os.path.join(DOCS, "evidence")


def require_ps3():
    if not PS3_ELF:
        raise SystemExit("[paths] this tool needs the PS3 debug ELF: set AC4_PS3_ELF")
    return PS3_ELF


def require_symbols():
    if not SYMBOLS:
        raise SystemExit("[paths] this tool needs the symbol manifests: set AC4_SYMBOLS")
    return SYMBOLS


def ensure_work():
    os.makedirs(WORK, exist_ok=True)
    return WORK


def ensure_evidence():
    os.makedirs(EVIDENCE, exist_ok=True)
    return EVIDENCE


def describe():
    rows = [
        ("repo", REPO), ("game", GAME_DIR), ("known exe", KNOWN_EXE),
        ("target exe", TARGET_EXE),
        ("ps3 elf", PS3_ELF or "(not found - set AC4_PS3_ELF)"),
        ("symbols", SYMBOLS or "(not found - set AC4_SYMBOLS)"),
        ("work", WORK), ("evidence", EVIDENCE),
    ]
    return "\n".join(f"  {k:11} {v}" for k, v in rows)


if __name__ == "__main__":
    print(describe())
