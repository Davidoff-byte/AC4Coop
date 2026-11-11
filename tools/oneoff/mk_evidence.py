import os, textwrap
D = r"C:\Users\Administrator\Documents\Default Project\projects\assassins-creed\bf-coop\ref\AC4Coop\decomp\evidence"
os.makedirs(D, exist_ok=True)
HDR = """{name} @ 0x{addr}  (size {size})   -- probe receipt, no cl 16.00.40219.01 on this machine
measured 2026-11-08 by ORCH against the pinned binary
  AC4BFSP.exe sha256 bc11ad7924e1eca7363ee86aa3332c05386d0a05896c0b7370131daee9ab5150
  candidate body: ac4-decomp\\{addr}.c (submitted verbatim)

claim
{claim}

cross-check (independent of the decompiler)
{cross}

limits
  probe arm only: this is UNVERIFIED-EVIDENCE, never a match. The byte arm needs the pinned
  compiler; when it exists, re-submit with --obj and the receipt re-derives both sides.
"""
items = {
 "679220": dict(name="ActionGate", size=84, claim=textwrap.dedent("""\
    sub_679220 is the state gate the action-guard hook patches. It begins with
    `mov ax,[ecx+0xFC]` (7 bytes) and returns 1 when (word & 7) == 0; the packed
    word is the same kind/role dword decoded elsewhere in this project."""),
   cross=textwrap.dedent("""\
    pe_probe on the pinned exe: 0x679220 = 66 8B 81 FC 00 00 00 66 83 E0 07 75 03 B0 01 C3
    (mov ax,[ecx+0xFC]); and the 14:23 crash run's sidecar:
      [AG] guard installed @ 0x00679220 (steal=7, resume=0x00679227)
      [AG] null target node=22102D50 outer=22102C70 a114=00000000 a118=22102C70 fc=0600
    i.e. the site resolves, the steal is one whole instruction, and the guard fires on a real
    null-target node in ordinary play.""")),
 "5FAC00": dict(name="BuildEntityWithSelector", size=94, claim=textwrap.dedent("""\
    sub_5FAC00 is the builder entry reached through vtable 0x1E57790 slot 17. It takes the
    out-Entity (a2) and the selector (a3) and drives the builder's own vtable slots +0x4C and
    +0x40, then calls sub_162BEE0/sub_8B2B10/sub_7A2380."""),
   cross=textwrap.dedent("""\
    pe_probe on the pinned exe: vtable 0x1E57790 +0x44 (slot 17) = 0x005FAC00, and the slot is
    read from .rdata, so the PC half is binary-verified. PS3 map has two builders
    (EntityBuilder 0x006aa328 / EntityGroupBuilder 0x006aa400) - which one this is stays open,
    because size alone may not decide it.""")),
 "19FB4C0": dict(name="SetLife", size=50, claim=textwrap.dedent("""\
    sub_19FB4C0 is the health setter: it stores the new value at byte +0x5C of the object as an
    int16 and, when the value is negative, takes the death path sub_A014A0(dword_4E1CC40, 0).
    This is the "SetLife(hp, -1) is the kill" used by D-016."""),
   cross=textwrap.dedent("""\
    the decompiled body stores `*(this + 46)` on an `__int16*`, i.e. +0x5C bytes, not +0x2E;
    the negative branch is the only path that calls sub_A014A0. Recorded because the same
    expression in a `_DWORD*` body would mean a different offset.""")),
 "73B4B0": dict(name="IsSoldierPredicate", size=31, claim=textwrap.dedent("""\
    sub_73B4B0 is the engine's soldier predicate: (word & 7) == 3 and (word & 0x1F8) == 8, i.e.
    kind 3 / role 1. It proves the +0x9C packing model used by EntityKindRole()."""),
   cross=textwrap.dedent("""\
    PS3 ELF: EntityDescriptorNPCSubType DIE 0x2BAEC3 -> 1 = Templar, 2 = Peasant, 3 = Thief.
    0x1F8 >> 3 = 0x3F gives the 6-bit role field at bit 3, matching the mod's decode.
    Also: PS3 EntityKind (0 None/1 Spawned/2 Picked/3 Player) is NOT this field - recorded as
    a conflict in ops/PS3-P1-RESULTS.md.""")),
 "54EC00": dict(name="IsCivilianPredicate", size=31, claim=textwrap.dedent("""\
    sub_54EC00 is the engine's civilian predicate: (word & 7) == 2 and (word & 0x1F8) == 0x10,
    i.e. kind 2 / role 2 (Peasant). Same packing, second data point."""),
   cross=textwrap.dedent("""\
    paired with sub_73B4B0 this fixes both the mask width and the field positions from two
    independent functions; the live 600-body census (all kind==2) is consistent with it.""")),
 "5FD730": dict(name="CrowdSpawnSlot39", size=95, claim=textwrap.dedent("""\
    sub_5FD730 is the crowd/free-roam spawn reached through vtable 0x1E57790 slot 39. It is a
    thin wrapper over sub_5FAC60 gated on the byte at dword_2AC1E68."""),
   cross=textwrap.dedent("""\
    pe_probe on the pinned exe: vtable 0x1E57790 +0x9C (slot 39) = 0x005FD730."""),),
}
for addr, it in items.items():
    p = os.path.join(D, addr + ".txt")
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(HDR.format(addr=addr, **it))
    print(p, os.path.getsize(p), "B")
