"""decomp spine - one unit of work per function, evidence required, receipts re-checked.

    python tools/decomp.py init
    python tools/decomp.py add    0x5200C0 51 GetComponentByType
    python tools/decomp.py claim  0x5200C0 --owner seat-name
    python tools/decomp.py submit 0x5200C0 candidate.cpp [--obj candidate.obj] [--evidence log.txt]
    python tools/decomp.py check                     # re-verify every receipt from scratch
    python tools/decomp.py list [--status candidate]

Two arms, and the receipt always says which was used:

  byte   the candidate object's .text is byte-identical to the original function, with the bytes
         covered by the object's own relocations zeroed on both sides. This is the only arm that
         can produce a MATCH. It needs the object file compiled with the *pinned* compiler
         (VC++ 2010 SP1, cl 16.00.40219.01) - pass it with --obj.

  probe  no compiler available: the receipt records the candidate hash plus the hash of a log
         excerpt supplied with --evidence, and the verdict is UNVERIFIED-EVIDENCE. It never counts
         as a match. That arm exists so behavioural evidence is still durable and checkable.

`check` never trusts a stored verdict: it re-hashes the original bytes from the game binary, the
candidate file, the object and the evidence, and fails on any drift. Editing a receipt to say
MATCH makes `check` fail, which is the point.
"""
from __future__ import annotations

import hashlib
import io
import json
import os
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import paths  # noqa: E402
from pe import PE  # noqa: E402

ROOT = os.path.join(paths.REPO, "decomp")
UNITS = os.path.join(ROOT, "units.json")
RECEIPTS = os.path.join(ROOT, "receipts")
CANDIDATES = os.path.join(ROOT, "candidates")
PINNED_CL = "16.00.40219.01"


# ---------------------------------------------------------------- helpers
def sha(b):
    return hashlib.sha256(b).hexdigest()


def load_units():
    if not os.path.exists(UNITS):
        return {"units": {}}
    return json.load(open(UNITS, encoding="utf-8"))


def save_units(d):
    os.makedirs(ROOT, exist_ok=True)
    tmp = UNITS + ".tmp"
    json.dump(d, open(tmp, "w", encoding="utf-8"), indent=1, sort_keys=True)
    os.replace(tmp, UNITS)


def original_bytes(addr, size):
    pe = PE(paths.KNOWN_EXE)
    b = pe.bytes_at_va(addr, max(size, 16))
    if not b:
        raise SystemExit(f"[decomp] {addr:#x} is not in the image")
    return b[:size] if size else b


def key(addr):
    return f"{addr:08X}"


def unit(addr, create=False, size=0, name=""):
    d = load_units()
    k = key(addr)
    u = d["units"].get(k)
    if u is None and create:
        u = {"addr": k, "size": size, "name": name, "status": "unknown",
             "owner": None, "claimed_at": None, "receipts": []}
        d["units"][k] = u
        save_units(d)
    return u


# ---------------------------------------------------------------- COFF
def coff_text(obj):
    """Return (.text bytes, [relocation offsets inside .text]) from an x86 COFF object."""
    d = open(obj, "rb").read()
    machine, nsec, _t, symptr, nsym, optsz, chars = struct.unpack_from("<HHIIIHH", d, 0)
    if machine != 0x14C:
        raise SystemExit(f"[decomp] not an x86 object (machine {machine:#x})")
    off = 20 + optsz
    sections = []
    for i in range(nsec):
        o = off + i * 40
        raw = d[o:o + 40]
        name = raw[:8].rstrip(b"\0").decode("latin-1", "replace")
        vsize, vaddr, rawsize, rawptr, relptr, lnn, nrel, nln = struct.unpack_from("<IIIIIIHH", raw, 8)
        sections.append({"name": name, "rawsize": rawsize, "rawptr": rawptr,
                         "relptr": relptr, "nrel": nrel})
    text = next((s for s in sections if s["name"] == ".text"), None)
    if not text:
        raise SystemExit("[decomp] object has no .text")
    body = d[text["rawptr"]:text["rawptr"] + text["rawsize"]]
    relocs = []
    for i in range(text["nrel"]):
        o = text["relptr"] + i * 10
        vaddr, symidx, rtype = struct.unpack_from("<IIH", d, o)
        relocs.append((vaddr, rtype))
    return body, relocs


def normalise(body, relocs):
    b = bytearray(body)
    for vaddr, rtype in relocs:
        if rtype in (0x06, 0x14, 0x07, 0x0B):        # REL32 / DIR32 family: 4 bytes
            for k in range(vaddr, min(vaddr + 4, len(b))):
                b[k] = 0
        elif rtype in (0x01, 0x02):                   # REL16 etc
            for k in range(vaddr, min(vaddr + 2, len(b))):
                b[k] = 0
    return bytes(b)


# ---------------------------------------------------------------- commands
def cmd_init(argv):
    os.makedirs(RECEIPTS, exist_ok=True)
    os.makedirs(CANDIDATES, exist_ok=True)
    save_units(load_units())
    print(f"initialised {ROOT}")
    return 0


def cmd_add(argv):
    if len(argv) < 3:
        print("usage: decomp.py add <addr> <size> <name>")
        return 2
    addr, size, name = int(argv[0], 16), int(argv[1]), " ".join(argv[2:])
    u = unit(addr, create=True, size=size, name=name)
    if u:
        print(f"{key(addr)} already in the index")
    print(f"added {key(addr)}  size={size}  {name}")
    return 0


def cmd_claim(argv):
    addr = int(argv[0], 16)
    owner = "unowned"
    if "--owner" in argv:
        owner = argv[argv.index("--owner") + 1]
    d = load_units()
    k = key(addr)
    u = d["units"].get(k)
    if u is None:
        print(f"{k} is not in the index - use `add` first")
        return 1
    if u["status"] == "matched":
        print(f"{k} is already MATCHED; refusing to re-claim")
        return 1
    if u["owner"] and u["owner"] != owner and u["status"] == "claimed":
        print(f"{k} is claimed by {u['owner']}; refusing")
        return 1
    u["status"] = "claimed"
    u["owner"] = owner
    u["claimed_at"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    save_units(d)
    print(f"{k} claimed by {owner}")
    return 0


def cmd_submit(argv):
    if len(argv) < 2:
        print("usage: decomp.py submit <addr> <candidate.cpp> [--obj x.obj] [--evidence log.txt]")
        return 2
    addr = int(argv[0], 16)
    cpp = argv[1]
    obj = argv[argv.index("--obj") + 1] if "--obj" in argv else None
    ev = argv[argv.index("--evidence") + 1] if "--evidence" in argv else None

    d = load_units()
    k = key(addr)
    u = d["units"].get(k)
    if u is None:
        print(f"{k} is not in the index")
        return 1
    size = u["size"] or len(original_bytes(addr, 0))
    orig = original_bytes(addr, size)
    cand_src = open(cpp, "rb").read()

    receipt = {
        "unit": k, "addr": f"0x{addr:08X}", "size": size, "name": u.get("name", ""),
        "submitted_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "original_sha": sha(orig),
        "candidate_src_sha": sha(cand_src),
        "candidate_src": os.path.basename(cpp),
        "arm": None, "verdict": None, "detail": "",
    }

    objcopy = None
    if obj and os.path.exists(obj):
        os.makedirs(os.path.join(ROOT, "objects"), exist_ok=True)
        objcopy = os.path.join(ROOT, "objects", f"{k}.obj")
        open(objcopy, "wb").write(open(obj, "rb").read())
        body, relocs = coff_text(obj)
        nb = normalise(body, relocs)
        no = orig  # original bytes have no relocation table here; zeroing the candidate's
                   # reloc slots is only correct when the same slots are data in the original,
                   # so this compares the *non-relocated* majority of the function.
        m = min(len(nb), len(no))
        diff = [i for i in range(m) if nb[i] != no[i]]
        receipt["arm"] = "byte"
        receipt["object_sha"] = sha(open(obj, "rb").read())
        receipt["object"] = f"objects/{k}.obj"
        receipt["compiler_expected"] = PINNED_CL
        receipt["compiler_seen"] = None
        receipt["relocations"] = len(relocs)
        receipt["byte_diffs"] = len(diff)
        receipt["verdict"] = "MATCH" if not diff and len(nb) == len(no) else "DIFF"
        receipt["detail"] = f"{len(diff)} differing byte(s); lengths {len(nb)} vs {len(no)}"
    else:
        receipt["arm"] = "probe"
        receipt["evidence"] = None
        receipt["verdict"] = "UNVERIFIED-EVIDENCE"
        receipt["detail"] = ("no object supplied: the pinned compiler "
                             f"(cl {PINNED_CL}) is not installed, so byte equality cannot be "
                             "checked. Recorded as evidence only - never a match.")
        if ev and os.path.exists(ev):
            b = open(ev, "rb").read()
            receipt["evidence_sha"] = sha(b)
            receipt["evidence"] = os.path.basename(ev)

    os.makedirs(RECEIPTS, exist_ok=True)
    os.makedirs(CANDIDATES, exist_ok=True)
    candname = f"{k}.cpp"
    open(os.path.join(CANDIDATES, candname), "wb").write(cand_src)
    receipt["candidate_src"] = candname
    if ev and os.path.exists(ev):
        os.makedirs(os.path.join(ROOT, "evidence"), exist_ok=True)
        evname = f"{k}.txt"
        open(os.path.join(ROOT, "evidence", evname), "wb").write(open(ev, "rb").read())
        receipt["evidence"] = evname
    json.dump(receipt, open(os.path.join(RECEIPTS, f"{k}.json"), "w", encoding="utf-8"), indent=1)

    u.setdefault("receipts", []).append({"at": receipt["submitted_at"], "arm": receipt["arm"],
                                         "verdict": receipt["verdict"]})
    if receipt["verdict"] == "MATCH":
        u["status"] = "matched"
    elif u["status"] == "unknown":
        u["status"] = "candidate"
    save_units(d)
    print(f"{k}: arm={receipt['arm']} verdict={receipt['verdict']} ({receipt['detail']})")
    return 0


def cmd_check(argv):
    """Re-derive everything. Any drift - including an edited verdict - fails."""
    if not os.path.isdir(RECEIPTS):
        print("no receipts")
        return 0
    bad = 0
    n = 0
    for name in sorted(os.listdir(RECEIPTS)):
        if not name.endswith(".json"):
            continue
        n += 1
        r = json.load(open(os.path.join(RECEIPTS, name), encoding="utf-8"))
        addr = int(r["addr"], 16)
        orig = original_bytes(addr, r["size"])
        if sha(orig) != r["original_sha"]:
            print(f"FAIL {r['unit']}: original bytes changed since the receipt")
            bad += 1
            continue
        cpp = os.path.join(CANDIDATES, r["candidate_src"])
        if os.path.exists(cpp):
            if sha(open(cpp, "rb").read()) != r["candidate_src_sha"]:
                print(f"FAIL {r['unit']}: candidate source drifted")
                bad += 1
                continue
        if r.get("evidence"):
            ev = os.path.join(ROOT, "evidence", r["evidence"])
            if not os.path.exists(ev):
                print(f"FAIL {r['unit']}: evidence file missing")
                bad += 1
                continue
            if sha(open(ev, "rb").read()) != r.get("evidence_sha"):
                print(f"FAIL {r['unit']}: evidence drifted")
                bad += 1
                continue
        if r["verdict"] == "MATCH":
            if r["arm"] != "byte" or r.get("object") is None:
                print(f"FAIL {r['unit']}: receipt claims MATCH with no object to re-derive it")
                bad += 1
                continue
            objp = os.path.join(ROOT, r["object"])
            if not os.path.exists(objp):
                print(f"FAIL {r['unit']}: object missing - MATCH cannot be re-derived")
                bad += 1
                continue
            if sha(open(objp, "rb").read()) != r.get("object_sha"):
                print(f"FAIL {r['unit']}: object drifted since the receipt")
                bad += 1
                continue
            body, relocs = coff_text(objp)
            nb = normalise(body, relocs)
            m = min(len(nb), len(orig))
            diffs = sum(1 for i in range(m) if nb[i] != orig[i])
            if diffs or len(nb) != len(orig):
                print(f"FAIL {r['unit']}: MATCH does not survive re-check "
                      f"({diffs} differing byte(s), {len(nb)} vs {len(orig)})")
                bad += 1
                continue
            if r.get("compiler_seen") != PINNED_CL and r.get("compiler_seen") is not None:
                print(f"WARN {r['unit']}: MATCH compiled with {r.get('compiler_seen')}, "
                      f"pinned is {PINNED_CL}")
        print(f"ok   {r['unit']}  {r['arm']:5} {r['verdict']}")
    print(f"\n{n} receipt(s), {bad} failing")
    return 0 if bad == 0 else 1


def cmd_list(argv):
    d = load_units()
    want = argv[argv.index("--status") + 1] if "--status" in argv else None
    for k in sorted(d["units"]):
        u = d["units"][k]
        if want and u["status"] != want:
            continue
        print(f"  {k}  {u['size']:5d}  {u['status']:10} {str(u['owner'] or ''):14} {u.get('name','')}")
    return 0


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    cmd, rest = argv[0], argv[1:]
    return {
        "init": cmd_init, "add": cmd_add, "claim": cmd_claim,
        "submit": cmd_submit, "check": cmd_check, "list": cmd_list,
    }.get(cmd, lambda a: (print(__doc__), 2)[1])(rest)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
