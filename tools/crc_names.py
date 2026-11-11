"""crc_names.py - resolve a component class id (CRC) back to its class name.

Proven 2026-11-08: the engine's component/service class id is
`zlib.crc32(class_name)` - `crc32("CSrvNPCHealth") == 1475276487`, the literal in
`sub_1371140`. So any `crc=` printed by the mod (BindDiag, ShipDump, the
health-service table) can be named by scanning the class-name strings that ship
inside AC4BFSP.exe.

    py -3.14 tools/crc_names.py 1475276487 1262645193 2036133539
    py -3.14 tools/crc_names.py --dump ../../ops/evidence/crc-names.tsv

The name table is the null-separated ASCII set in the image; we only trust a hit
when exactly one string hashes to the requested value.
"""
import argparse, os, re, sys, zlib

EXE = r"D:\SteamLibrary\steamapps\common\Assassin's Creed IV Black Flag\AC4BFSP.exe"
PINNED = "bc11ad7924e1eca7363ee86aa3332c05386d0a05896c0b7370131daee9ab5150"
PRINT = re.compile(rb"[ -~]{4,80}")


def load_names(path=EXE):
    blob = open(path, "rb").read()
    by_crc = {}
    for m in PRINT.finditer(blob):
        s = m.group(0).decode("ascii", "replace")
        c = zlib.crc32(s.encode("ascii", "replace")) & 0xFFFFFFFF
        by_crc.setdefault(c, set()).add(s)
    return blob, by_crc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("crcs", nargs="*", help="decimal or 0x-hex class ids")
    ap.add_argument("--dump", metavar="TSV", help="write crc<TAB>name for every unambiguous id")
    a = ap.parse_args()

    blob, by_crc = load_names()
    import hashlib
    h = hashlib.sha256(blob).hexdigest()
    print(f"exe sha256 {h}" + ("" if h == PINNED else "  ** NOT THE PINNED BINARY **"))

    for raw in a.crcs:
        c = int(raw, 0) & 0xFFFFFFFF
        names = sorted(by_crc.get(c, ()))
        if not names:
            print(f"{c:>12}  (no string in the image hashes to this)")
        elif len(names) == 1:
            print(f"{c:>12}  {names[0]}")
        else:
            print(f"{c:>12}  AMBIGUOUS: {', '.join(names[:6])}")

    if a.dump:
        n = 0
        with open(a.dump, "w", encoding="utf-8", newline="\n") as f:
            f.write("crc\tname\n")
            for c in sorted(by_crc):
                names = by_crc[c]
                if len(names) != 1:
                    continue
                nm = next(iter(names))
                if not (nm[0].isalpha() or nm[0] == "_"):
                    continue
                f.write(f"{c}\t{nm}\n")
                n += 1
        print(f"wrote {a.dump} ({n} unambiguous ids)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
