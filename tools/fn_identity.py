"""Measure how many of the plugin's sites are byte-identical between two builds.

The FNV-style method pins the original compiler and proves understanding by byte equality. The
same test answers a different question for us: since both AC4 builds came from the same toolchain,
any function whose *source* did not change compiles to identical bytes. So an exact whole-function
match means "unchanged"; a miss means "genuinely different and must be re-derived".

    python fn_identity.py --target <exe> [--sites 0x...,0x...] [--map docs/evidence/sig_map_v3.json]

Prints, per site: identical (target VA) / changed / unavailable.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths  # noqa: E402
from pe import PE  # noqa: E402

N_BYTES = 256


def body(pe, va, limit=N_BYTES):
    """Bytes from va up to the first ret (inclusive), capped."""
    code = pe.bytes_at_va(va, limit)
    if not code:
        return b""
    i = code.find(b"\xC3")          # ret
    j = code.find(b"\xC2")          # ret imm16
    end = len(code)
    if i >= 0:
        end = min(end, i + 1)
    if j >= 0:
        end = min(end, j + 3)
    return code[:end]


def main(argv):
    target = None
    sites = []
    mapfile = None
    for i, a in enumerate(argv):
        if a == "--target":
            target = argv[i + 1]
        elif a == "--sites":
            sites = [int(x, 16) for x in argv[i + 1].split(",") if x]
        elif a == "--map":
            mapfile = argv[i + 1]
    if not target:
        print(__doc__)
        return 2

    kpe, tpe = PE(paths.KNOWN_EXE), PE(target)
    if not sites and mapfile and os.path.exists(mapfile):
        rows = json.load(open(mapfile, encoding="utf-8"))["rows"]
        sites = [r["known"] for r in rows]

    same, changed, none = [], [], []
    for va in sites:
        b = body(kpe, va)
        if len(b) < 8:
            none.append(va)
            continue
        pos = tpe.data.find(b)
        if pos < 0:
            changed.append(va)
        else:
            tva = tpe.off_to_va(pos)
            # require the match to be inside code and unique
            second = tpe.data.find(b, pos + 1)
            same.append((va, tva, len(b), second < 0))

    tot = len(sites)
    print(f"sites {tot}   identical {len(same)}   changed {len(changed)}   no-body {len(none)}")
    uniq = [s for s in same if s[3]]
    print(f"  of the identical ones, unique in target: {len(uniq)}")
    print("\nidentical (unchanged source):")
    for va, tva, n, unique in same:
        flag = "" if unique else "  (multiple hits)"
        print(f"  {va:#9x} -> {tva:#9x}  body {n:4d} B{flag}")
    print("\nchanged (source differs, needs re-derivation):")
    print("  " + ", ".join(f"{v:#x}" for v in changed))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
