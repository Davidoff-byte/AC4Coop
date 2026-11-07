"""Disassemble the PS3 Gamer::RegisterAvatarEntity / UnRegisterAvatarEntity."""
import re, sys
from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN

MAP = r"D:\Games\acbf_ps3_gold\scimitar_final.map"
ELF = r"D:\Games\acbf_ps3_gold\scimitar_final.elf"
LINE = re.compile(r"^\s*([0-9a-fA-F]{8})\s+([0-9a-fA-F]{8})\s+\d+\s+(.*)$")

def syms():
    out = []
    with open(MAP, encoding="latin-1", errors="replace") as f:
        for line in f:
            m = LINE.match(line)
            if not m: continue
            addr, size, tail = int(m.group(1),16), int(m.group(2),16), m.group(3).strip()
            name = tail if "scimitar::" in tail else (tail.split()[-1] if tail else "")
            if name: out.append((addr,size,name))
    return out

def reader():
    elf = ELFFile(open(ELF,"rb")); segs=[]
    for s in elf.iter_sections():
        if s["sh_type"]=="SHT_PROGBITS" and (s["sh_flags"] & 0x4):
            segs.append((s["sh_addr"], s["sh_addr"]+s["sh_size"], s.data()))
    def read(va,n):
        for lo,hi,d in segs:
            if lo <= va < hi: return d[va-lo:va-lo+n]
        return b""
    return read

S = syms()
print("symbols parsed:", len(S))
targets = [s for s in S if any(k in s[2] for k in
           ("RegisterAvatarEntity","GamerCollection","GamerWorldInstance","CreateGamer","AddGamer"))]
print("gamer-related:", len(targets))
for a,z,n in sorted(targets)[:25]: print(f"  {a:#010x} {z:5d}  {n[:90]}")

rd = reader(); md = Cs(CS_ARCH_PPC, CS_MODE_32|CS_MODE_BIG_ENDIAN); md.detail=True
want = [s for s in targets if "RegisterAvatarEntity" in s[2] or "UnRegisterAvatarEntity" in s[2]]
for a,z,n in want[:2]:
    print(f"\n=== {n}")
    c=0
    for ins in md.disasm(rd(a, max(z,0x40)), a):
        calls = ""
        if ins.mnemonic in ("bl","b","bla") and ins.op_str:
            calls = "   ; " + ins.op_str
        print(f"  {ins.address:#010x}  {ins.mnemonic:8} {ins.op_str}{calls}")
        c+=1
        if c>=64: print("  ..."); break
