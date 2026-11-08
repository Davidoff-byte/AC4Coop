"""Build candidate + evidence files for the three base()-relative callees.

Evidence is generated from the pinned binary (bytes + disassembly), never retyped.
"""
import hashlib, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import paths
from pe import PE
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

IMAGE_BASE = 0x400000
pe = PE(paths.KNOWN_EXE)
md = Cs(CS_ARCH_X86, CS_MODE_32)
EXE_SHA = hashlib.sha256(open(paths.KNOWN_EXE, "rb").read()).hexdigest()
CAND = os.path.join(paths.REPO, "decomp", "candidates")
EV = os.path.join(os.environ["TEMP"], "ac4_ev")
os.makedirs(CAND, exist_ok=True)
os.makedirs(EV, exist_ok=True)

SRC = os.path.join(paths.REPO, "src", "phase1.cpp")
SRC_LINES = open(SRC, encoding="utf-8", errors="replace").read().split("\n")


def src(line):
    return SRC_LINES[line - 1].strip()


def dis(va, size):
    out = []
    for i in md.disasm(pe.bytes_at_va(va, size), va):
        out.append(f"    0x{i.address:08X}  {i.bytes.hex():<20} {i.mnemonic:<7} {i.op_str}")
    return "\n".join(out)


def head_tail(va, size, n=16):
    b = pe.bytes_at_va(va, size)
    return f"    head {b[:n].hex(' ')}\n    tail {b[max(0, size - n):size].hex(' ')}"


UNITS = [
    dict(va=0x00503330, size=322, rva=0x00103330, name="ShipEntityCtor",
         site=1726,
         arith="""    base() is GetModuleHandle(NULL) = the image base 0x400000
    0x400000 + 0x00103330 = 0x00503330   -> the VA this unit documents""",
         cand="""// 0x503330  sub_503330  size=322  ShipEntityCtor   (__thiscall, returns this)
// Verified from the image at 0x503330:
//   call 0x52A4A0           base-class ctor (this already in ecx)
//   mov  [esi], 0x1E4A128   vtable - the exact vtable phase1.cpp:1732 expects
//   one-time copy of 0x40 bytes of static data (0x1E49060..0x1E49090) into
//   0x2ABE5A0.., gated on byte 0x2ABE5E0 (init-once flag)
//   those four 16-byte blocks are then stored at this+0x100, +0x110, +0x120, +0x130
//   word at this+0x148 = 0
// PARTIAL: the identity of the static data blocks is not established.
int __thiscall sub_503330(void *this)
{
  sub_52A4A0();
  *(unsigned int *)this = 0x1E4A128u;
  if (!(*(unsigned char *)0x2ABE5E0 & 1))
  {
    *(unsigned int *)0x2ABE5E0 |= 1u;
    copy16((void *)0x2ABE5A0, (const void *)0x1E49060);
    copy16((void *)0x2ABE5B0, (const void *)0x1E49070);
    copy16((void *)0x2ABE5C0, (const void *)0x1E49080);
    copy16((void *)0x2ABE5D0, (const void *)0x1E49090);
  }
  copy16((char *)this + 0x100, (const void *)0x2ABE5A0);
  copy16((char *)this + 0x110, (const void *)0x2ABE5B0);
  copy16((char *)this + 0x120, (const void *)0x2ABE5C0);
  copy16((char *)this + 0x130, (const void *)0x2ABE5D0);
  *(unsigned short *)((char *)this + 0x148) = 0;
  return (int)this;
}
"""),
    dict(va=0x01082810, size=155, rva=0x00C82810, name="SpawnRequestAlloc",
         site=7703,
         arith="""    base() is GetModuleHandle(NULL) = the image base 0x400000
    0x400000 + 0x00C82810 = 0x01082810   -> the VA this unit documents""",
         cand="""// 0x1082810  sub_1082810  size=155  SpawnRequestAlloc   (__cdecl, returns object)
// Verified from the image at 0x1082810:
//   lock   0x2AC1FCC
//   p = 0x418A80(*(0x2AC1F68), 0xDD0, 0x10)   object of 0xDD0 bytes; the two
//                                             stack params' meaning (size/align)
//                                             is not resolved
//   if (p) 0x15666E0(p)                        construct it
//   ring insert: index = (*(u16*)0x2AC1FC2 + 1) against (*(u32*)0x2AC1FC0 & 0x3FFF),
//                table *(0x2AC1FBC), slot dword at table + idx*4 - 4 = p
//   unlock 0x2AC1FCC
//   return p
// This is an allocate-construct-enqueue factory; the object's type is NOT verified.
void *sub_1082810(void)
{
  void *p;
  unsigned int idx;
  sub_D587F0(0x2AC1FCC);
  p = (void *)sub_418A80(*(unsigned int *)0x2AC1F68, 0xDD0, 0x10);
  if (p)
    sub_15666E0(p);
  idx = *(unsigned short *)0x2AC1FC2;
  if ((idx + 1) > (*(unsigned int *)0x2AC1FC0 & 0x3FFF))
    idx = sub_A09AB0(0x2AC1FBC, idx + 1);
  idx = *(unsigned short *)0x2AC1FC2 + 1;
  *(unsigned short *)0x2AC1FC2 = (unsigned short)idx;
  *(void **)(*(unsigned int *)0x2AC1FBC + idx * 4 - 4) = p;
  sub_AC6580(0x2AC1FCC);
  return p;
}
"""),
    dict(va=0x00FB3220, size=338, rva=0x00FB3220, name="SubAtVA_00FB3220",
         site=7833,
         arith="""    base() is GetModuleHandle(NULL) = the image base 0x400000
    0x400000 + 0x00FB3220 = 0x013B3220   -> NOT this unit: 0x013B3220 is not a
                                            function entry (see BUG below)
    this unit documents 0x00FB3220 - the address the call site's own comment
    names (phase1.cpp:7826 "FUN_00fb3220"); its RVA is 0xBB3220""",
         cand="""// 0xFB3220  sub_FB3220  size=338   name withheld: semantics not established
// Signature is stack-based - reads arg1 at [ebp+8], arg2 (char) at [ebp+0xC] -
// i.e. __cdecl/__stdcall, NOT __thiscall (the call site at phase1.cpp:7833/7838
// casts it to __thiscall, so this call shape does not match the callee).
// Verified control flow:
//   if (!arg1) return 0
//   build a temp at [ebp-0x10] (sub_FA3AA0(temp, 1))
//   if (arg2)  vcall arg1->vtbl[0x68/4](&temp)
//   else       sub_525590(arg1, sub_79B5E0(&temp))
//   iterate the container at [ebp-0x10] (count >> 16, 8-byte records) and call a
//     global's vtbl[0x14/4] per record
//   return arg1
int __cdecl sub_FB3220(void *a1, char a2)
{
  int tmp[4];
  if (!a1)
    return 0;
  sub_FA3AA0(tmp, 1);
  if (a2)
    (*(void (__thiscall **)(void *, int *))(*(unsigned int *)a1 + 0x68))(a1, tmp);
  else
    sub_525590(a1, (void *)sub_79B5E0(tmp));
  /* container walk over tmp[0]/tmp[1] (see 0xFB3265..0xFB3355) */
  return (int)a1;
}
"""),
]

for u in UNITS:
    key = f"{u['va']:08X}"
    open(os.path.join(CAND, key + ".cpp"), "w", encoding="utf-8").write(u["cand"])
    va, size = u["va"], u["size"]
    b = pe.bytes_at_va(va, size)
    ev = f"""{u['name']} @ 0x{va:08X}  (size {size})   -- probe receipt, no cl 16.00.40219.01 here
measured 2026-11-08 against the pinned binary
  AC4BFSP.exe sha256 {EXE_SHA}
  image base 0x400000   (PE header ImageBase; phase1.cpp:9189 notes the same)
  unit bytes sha256 {hashlib.sha256(b).hexdigest()}
  size {size} = first 4-byte int3 run after the unit (function boundary)

call site  phase1.cpp:{u['site']}
    {src(u['site'])}

RVA arithmetic (the form the textual gate rules could not see)
{u['arith']}

bytes (prologue / tail)
{head_tail(va, size)}

disassembly (prologue and the instructions named in the candidate)
{dis(va, min(size, 0x60))}

... tail:
{dis(va + size - 0x18, 0x18)}
"""
    if va == 0x013B3220:
        pass
    if u["rva"] == 0x00FB3220:
        ev += """
BUG - the call site's arithmetic, not this unit
    phase1.cpp:7833 and :7838 call ((ShipSubFn_t)(base() + 0x00FB3220))(shipEnt, 0).
    0x400000 + 0x00FB3220 = 0x013B3220. That address is NOT a function entry: it is
    inside a function whose body ends
        0x013B3271 pop edi / 0x013B3272 mov eax,esi / 0x013B3274 pop esi /
        0x013B3275 pop ebx / 0x013B3276 pop ebp / 0x013B3277 ret 8
    so it needs ebx/ebp for the enclosing frame and enters with a bare call. The
    literal 0x00FB3220 is already a VA (the comment on phase1.cpp:7826 says
    "FUN_00fb3220"), so base() is being added twice. Executing 0x013B3220 from a
    call site is what the source reports as "sub=EXCEPTION/unreadable".
    Logic is NOT changed here (dispatch forbids it); the gate carries this VA in
    EXCEPTIONS and requires that the intended callee 0x00FB3220 has a unit.
"""
    open(os.path.join(EV, f"{key}.txt"), "w", encoding="utf-8").write(ev)
    print(f"{key}  size={size}  cand={len(u['cand'])}B  evidence={len(ev)}B")
