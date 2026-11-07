"""Find candidate "return a global pointer" accessors in the PC build.

The PS3 GamerWorldInstance accessor is exactly:

    lis r9, 0x258
    lwz r3, 0x41a8(r9)      ; return *(0x25841a8)
    blr

On x86 that is `A1 imm32 C3` (mov eax, [imm32]; ret) or `8B 05 imm32 C3`. This lists every such
accessor whose target sits in a writable section, which is the shortlist the GamerWorldInstance
global must come from.

    python find_global_getters.py
    python find_global_getters.py --pattern "A1"
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths  # noqa: E402
from pe import PE  # noqa: E402

PATS = [(b"\xA1", 1), (b"\x8B\x05", 2)]   # mov eax,[imm32] ; mov eax,[imm32]


def main(argv):
    pe = PE(paths.KNOWN_EXE)
    code = [(s["vaddr"], s["rawptr"], s["rawsize"]) for s in pe.sections
            if s["chars"] & 0x20000000]
    data_ranges = []
    for s in pe.sections:
        if s["chars"] & 0x80000000 or s["chars"] & 0x40000000:      # writable
            data_ranges.append((pe.image_base + s["vaddr"],
                                pe.image_base + s["vaddr"] + max(s["vsize"], s["rawsize"])))

    def in_data(va):
        return any(lo <= va < hi for lo, hi in data_ranges)

    hits = []
    for vaddr, rawptr, rawsize in code:
        blob = pe.data[rawptr:rawptr + rawsize]
        for pat, _n in PATS:
            start = 0
            while True:
                i = blob.find(pat, start)
                if i < 0:
                    break
                start = i + 1
                off = i + len(pat)
                if off + 4 >= len(blob):
                    continue
                if blob[off + 4] != 0xC3:          # require ret straight after
                    continue
                target = struct.unpack_from("<I", blob, off)[0]
                if not in_data(target):
                    continue
                fn = pe.image_base + vaddr + i
                hits.append((fn, target))
    print(f"accessors returning a global pointer: {len(hits)}")
    # how often is each one called?  xref density is a weak but useful filter
    for fn, target in hits[:40]:
        print(f"  fn {fn:#010x}  -> global {target:#010x}")
    if len(hits) > 40:
        print(f"  ... {len(hits)-40} more")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
