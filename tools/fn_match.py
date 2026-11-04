"""Function-level relocation between two builds, without a disassembler database.

For a known-build address:
  1. find the function start (walk back to the nearest plausible prologue),
  2. measure the function (disassemble to the first ret / next prologue),
  3. normalise it to (mnemonic, operand-kind) tokens,
  4. if the body is a thunk (ends in jmp), follow the target and repeat,
  5. score the template against every prologue start in the target build, keeping only
     candidates whose size is within 35% (size is the strongest cheap discriminator).

    python fn_match.py --target <exe> --sites "0x54d4d0,0x70e810" [--min-score 0.75]
"""

# --- path resolution (see tools/paths.py; override with AC4_* env vars) ---
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import paths
import os
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE  # noqa: E402

KNOWN = paths.KNOWN_EXE
PROLOGUES = (b"\x55\x8b\xec", b"\x8b\xff\x55\x8b\xec", b"\x53\x8b\xdc", b"\x53", b"\x56",
             b"\x57", b"\x83\xec", b"\x81\xec", b"\x6a\xff\x68", b"\x8b\xc1", b"\x8b\x44\x24")
MAX_INSNS = 240


def code_blob(pe):
    for s in pe.sections:
        if s["chars"] & 0x20000000:
            return s, pe.data[s["rawptr"]: s["rawptr"] + s["rawsize"]]
    raise ValueError("no code section")


def is_prologue(pe, va):
    b = pe.bytes_at_va(va, 5)
    return any(b.startswith(p) for p in PROLOGUES)


def fn_start(pe, va, limit=0x800):
    """Walk back to the nearest prologue."""
    for d in range(0, limit, 1):
        if is_prologue(pe, va - d):
            return va - d
    return va


def normalise(md, pe, va, max_insns=MAX_INSNS):
    code = pe.bytes_at_va(va, 4096)
    if not code:
        return [], 0, None
    out, size, jmp_target = [], 0, None
    for ins in md.disasm(code, va):
        ops = []
        for op in ins.operands:
            ops.append({1: "r", 2: "i", 3: "m"}.get(op.type, "?"))
        out.append(ins.mnemonic + ":" + ",".join(ops))
        size += ins.size
        if ins.mnemonic == "ret" or ins.mnemonic.startswith("ret"):
            break
        if ins.mnemonic == "jmp" and ins.operands and ins.operands[0].type == 2:
            jmp_target = ins.operands[0].imm
            break
        if len(out) >= max_insns:
            break
    return out, size, jmp_target


def score(a, b):
    if not a or not b:
        return 0.0
    common = min(len(a), len(b))
    same = sum(1 for i in range(common) if a[i] == b[i])
    return same / max(len(a), len(b))


def main(argv):
    target, sites, min_score = None, [], 0.72
    for i, a in enumerate(argv):
        if a == "--target" and i + 1 < len(argv):
            target = argv[i + 1]
        if a == "--sites" and i + 1 < len(argv):
            sites = [int(x, 16) for x in argv[i + 1].split(",") if x]
        if a == "--min-score" and i + 1 < len(argv):
            min_score = float(argv[i + 1])
    if not target or not sites:
        print(__doc__)
        return 2

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    kpe, tpe = PE(KNOWN), PE(target)

    # candidate starts + sizes in the target
    tsec, tblob = code_blob(tpe)
    cands = []
    for pat in PROLOGUES:
        s = 0
        while True:
            i = tblob.find(pat, s)
            if i < 0:
                break
            va = tpe.image_base + tsec["vaddr"] + i
            toks, size, _ = normalise(md, tpe, va)
            if toks:
                cands.append((va, size, toks))
            s = i + 1
    print(f"candidate functions in target: {len(cands)}")

    for va in sites:
        start = fn_start(kpe, va)
        toks, size, jmp = normalise(md, kpe, start)
        note = ""
        if jmp and len(toks) <= 6:
            note = f" (thunk -> {jmp:#x}, following)"
            start2 = fn_start(kpe, jmp)
            toks, size, jmp2 = normalise(md, kpe, start2)
        print(f"\n{va:#x}  start={start:#x}  size={size}  insns={len(toks)}{note}")
        hits = []
        for cva, csize, ctoks in cands:
            if size and (csize < size * 0.65 or csize > size * 1.35):
                continue
            sc = score(toks, ctoks)
            if sc >= min_score:
                hits.append((sc, cva, csize))
        hits.sort(reverse=True)
        for sc, cva, csize in hits[:5]:
            print(f"   score={sc:.3f}  {cva:#x}  size={csize}")
        if not hits:
            print("   no candidate above threshold")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
