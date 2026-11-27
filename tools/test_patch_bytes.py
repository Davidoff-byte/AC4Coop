#!/usr/bin/env python3
"""test_patch_bytes.py - I-702: the expected-original-byte tables in the patchers must match the pinned exe.

The walk_guard leaf table carried 0x88 where the exe has 0x89 (fixed at 4a60604) and only the game noticed:
verify_signatures.py checks the deref_guard sites but not walk_guard's tables. This test parses the byte
tables straight out of the SOURCE (so an edit to a table is what gets tested, not a copy of it) and compares
each one with the bytes of the on-disk exe at the VA the patcher verifies them at.

  py tools\\test_patch_bytes.py            exit 0 = every table matches, 1 = a mismatch or a parse miss

Exe: paths.KNOWN_EXE (env AC4_KNOWN_EXE, or AC4BFSP.exe under AC4_GAME_DIR). Read-only.

COVERED (each parsed table must yield the expected number of entries, so a regex miss FAILS, never passes):
  src/walk_guard.cpp   PROLOGUE / RESUME (+8) / EPILOGUE (+epilogueOff) for every kTargets row,
                       stolen[stolenLen] / epilogue (+epilogueOff) for every kVecTargets row,
                       REL_STOLEN @ REL_SITE, REL_RESUMEB @ REL_RESUME
  src/deref_guard.cpp  EXP_114 / EXP_118 at LITERAL_FUNC_A/B + off for every kSites row, PATTERN_FUNC_A/B at
                       the literal bases, InstallStaleTarget's expSite/expCont/expSkip, the NR call site
                       (E8 rel32 -> LITERAL_NR_CALLEE), NR_EXP_CONT, NR_EXP_EPILOGUE, NO_PROLOGUE
NOT COVERED (listed at the end of every run): every other `memcmp((const void*)site, table, n)` in src/.
"""
from __future__ import annotations

import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths  # noqa: E402

IMAGE_BASE = 0x00400000
SRC = paths.SRC


class Pe:
    def __init__(self, data: bytes):
        self.data = data
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        nsec = struct.unpack_from("<H", data, pe + 6)[0]
        optsz = struct.unpack_from("<H", data, pe + 20)[0]
        self.base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
        self.secs = []
        sh = pe + 24 + optsz
        for i in range(nsec):
            vsz, va, rsz, raw = struct.unpack_from("<IIII", data, sh + 40 * i + 8)
            self.secs.append((va, max(vsz, rsz), raw, rsz))

    def read(self, va: int, n: int) -> bytes:
        rva = va - self.base
        for sva, size, raw, rsz in self.secs:
            if sva <= rva and rva + n <= sva + size:
                o = rva - sva
                if o + n > rsz:
                    raise ValueError(f"VA 0x{va:08X} is in uninitialised section data")
                return self.data[raw + o: raw + o + n]
        raise ValueError(f"VA 0x{va:08X} is not inside any section")


def src(name: str) -> str:
    with open(os.path.join(SRC, name), encoding="utf-8", errors="replace") as f:
        return f.read()


def hexbytes(s: str) -> bytes:
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{1,2})\b", s))


def array(text: str, name: str) -> bytes:
    m = re.search(r"\bBYTE\s+" + re.escape(name) + r"\s*\[[^\]]*\]\s*=\s*\{([^}]*)\}", text)
    if not m:
        raise LookupError(f"byte table {name} not found")
    return hexbytes(m.group(1))


def const(text: str, name: str) -> int:
    m = re.search(r"\b" + re.escape(name) + r"\s*=\s*(0x[0-9A-Fa-f]+)", text)
    if not m:
        raise LookupError(f"constant {name} not found")
    return int(m.group(1), 16)


def pattern_matches(pe: Pe, va: int, pat: str) -> bool:
    toks = pat.split()
    got = pe.read(va, len(toks))
    return all(t == "??" or int(t, 16) == b for t, b in zip(toks, got))


class Run:
    def __init__(self, pe: Pe):
        self.pe, self.ok, self.fail = pe, 0, 0

    def bytes_at(self, label: str, va: int, want: bytes):
        try:
            got = self.pe.read(va, len(want))
        except ValueError as e:
            self.bad(f"{label}: {e}")
            return
        if got == want and want:
            self.ok += 1
            print(f"  PASS {label} @ 0x{va:08X}: {want.hex(' ').upper()}")
        else:
            self.bad(f"{label} @ 0x{va:08X}: table {want.hex(' ').upper()} != exe {got.hex(' ').upper()}")

    def check(self, label: str, cond: bool, detail: str = ""):
        if cond:
            self.ok += 1
            print(f"  PASS {label}")
        else:
            self.bad(f"{label} {detail}")

    def bad(self, msg: str):
        self.fail += 1
        print(f"  FAIL {msg}")


def walk_guard(r: Run):
    print("src/walk_guard.cpp")
    t = src("walk_guard.cpp")
    pro, res, epi = array(t, "PROLOGUE"), array(t, "RESUME"), array(t, "EPILOGUE")
    rows = re.findall(r'\{\s*"(sub_\w+)",\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*&g_hits\w+\s*\}', t)
    r.check("kTargets rows parsed == 2", len(rows) == 2, f"(got {len(rows)})")
    for name, va, eoff in rows:
        va, eoff = int(va, 16), int(eoff, 16)
        r.bytes_at(f"{name} PROLOGUE", va, pro)
        r.bytes_at(f"{name} RESUME", va + 8, res)
        r.bytes_at(f"{name} EPILOGUE", va + eoff, epi)
    vec = re.findall(r'\{\s*"(sub_\w+)",\s*(0x[0-9A-Fa-f]+),\s*\{([^}]*)\},\s*(\d+),\s*(\d+),\s*(\d+),\s*'
                     r'(0x[0-9A-Fa-f]+),\s*\{([^}]*)\},\s*&g_hits\w+\s*\}', t)
    r.check("kVecTargets rows parsed == 3", len(vec) == 3, f"(got {len(vec)})")
    for name, va, stolen, slen, _out, _ret, eoff, ep in vec:
        va, eoff, slen = int(va, 16), int(eoff, 16), int(slen)
        sb = hexbytes(stolen)
        r.check(f"{name} stolenLen {slen} == table length", len(sb) == slen, f"(table has {len(sb)})")
        r.bytes_at(f"{name} stolen", va, sb[:slen])
        r.bytes_at(f"{name} epilogue", va + eoff, hexbytes(ep))
    r.bytes_at("REL_STOLEN", const(t, "REL_SITE"), array(t, "REL_STOLEN"))
    r.bytes_at("REL_RESUMEB", const(t, "REL_RESUME"), array(t, "REL_RESUMEB"))


def deref_guard(r: Run):
    print("src/deref_guard.cpp")
    t = src("deref_guard.cpp")
    exp = {"EXP_114": array(t, "EXP_114"), "EXP_118": array(t, "EXP_118")}
    base = {"FUNC_A": const(t, "LITERAL_FUNC_A"), "FUNC_B": const(t, "LITERAL_FUNC_B")}
    for f in ("A", "B"):
        pm = re.search(r'PATTERN_FUNC_' + f + r'\s*=\s*"([^"]+)"', t)
        r.check(f"PATTERN_FUNC_{f} matches at 0x{base['FUNC_' + f]:08X}",
                bool(pm) and pattern_matches(r.pe, base["FUNC_" + f], pm.group(1)))
    rows = re.findall(r'\{\s*(FUNC_[AB]),\s*(0x[0-9A-Fa-f]+),\s*(EXP_11[48]),\s*0x11[48],', t)
    r.check("kSites rows parsed == 6", len(rows) == 6, f"(got {len(rows)})")
    for fn, off, e in rows:
        r.bytes_at(f"{fn}+{off} {e}", base[fn] + int(off, 16), exp[e])
    stale = {"expSite": "LITERAL_STALE_SITE", "expCont": "LITERAL_STALE_CONT", "expSkip": "LITERAL_STALE_SKIP"}
    for arr, va in stale.items():
        r.bytes_at(f"stale {arr}", const(t, va), array(t, arr))
    site, callee = const(t, "LITERAL_NR_SITE"), const(t, "LITERAL_NR_CALLEE")
    b = r.pe.read(site, 5)
    dest = (site + 5 + struct.unpack("<i", b[1:5])[0]) & 0xFFFFFFFF
    r.check(f"NR site 0x{site:08X} is E8 -> 0x{callee:08X}", b[0] == 0xE8 and dest == callee,
            f"(got {b.hex(' ').upper()} -> 0x{dest:08X})")
    r.bytes_at("NR_EXP_CONT", const(t, "LITERAL_NR_CONT"), array(t, "NR_EXP_CONT"))
    r.bytes_at("NR_EXP_EPILOGUE", const(t, "LITERAL_NR_EPILOGUE"), array(t, "NR_EXP_EPILOGUE"))
    r.bytes_at("NO_PROLOGUE", const(t, "LITERAL_NO_FUNC"), array(t, "NO_PROLOGUE"))


def not_covered():
    out = []
    for fn in sorted(os.listdir(SRC)):
        if not fn.endswith(".cpp") or fn in ("walk_guard.cpp", "deref_guard.cpp"):
            continue
        for i, line in enumerate(src(fn).splitlines(), 1):
            if re.search(r"memcmp\(\s*\(const void\*\)|memcmp\((site|sgm|va|p),", line):
                out.append(f"{fn}:{i}")
    return out


def main() -> int:
    exe = paths.KNOWN_EXE
    with open(exe, "rb") as f:
        pe = Pe(f.read())
    print(f"exe: {exe} (ImageBase 0x{pe.base:08X})")
    if pe.base != IMAGE_BASE:
        print(f"FAIL: ImageBase 0x{pe.base:08X} != 0x{IMAGE_BASE:08X}")
        return 1
    r = Run(pe)
    for part in (walk_guard, deref_guard):
        try:
            part(r)
        except (LookupError, ValueError) as e:
            r.bad(f"{part.__name__}: {e}")
    nc = not_covered()
    print(f"NOT COVERED ({len(nc)} memcmp patch-site checks in other files): {', '.join(nc)}")
    print(f"RESULT: {'PASS' if r.fail == 0 else 'FAIL'} ({r.ok} passed, {r.fail} failed)")
    return 0 if r.fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
