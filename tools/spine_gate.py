"""spine_gate.py - the hard gate for the decompilation spine.

Two rules, both machine-checked, exit 0 only when both hold:

  1. RECEIPTS RE-DERIVE.  Every receipt in decomp/receipts/ is re-checked by
     `decomp.py check` against the pinned binary. A drifted candidate, a missing
     evidence file or a hand-edited verdict fails.
  2. NO UNDOCUMENTED ENGINE CALL.  Every direct call into the game made from the
     plugin (`))0xADDR)(` cast-and-call form) must have a unit in
     decomp/units.json. If we call an engine function, it gets a receipt - that is
     the rule that keeps "I called it and it seemed fine" out of the codebase.

Usage:
    py tools/spine_gate.py            # check, human output
    py tools/spine_gate.py --json     # machine output (used by the audit)
"""
import argparse, json, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                    # ref/AC4Coop
UNITS = os.path.join(ROOT, "decomp", "units.json")
SRC = os.path.join(ROOT, "src")

# `...cast...))0x19FB4C0)(args)`  -> an absolute call into the game image
CALL_RE = re.compile(r"\)\s*0x([0-9A-Fa-f]{6,8})\s*\)\s*\(")


def load_units():
    with open(UNITS, encoding="utf-8") as f:
        d = json.load(f)
    return {k.upper().lstrip("0").zfill(8): v for k, v in d.get("units", {}).items()}


def scan_calls():
    hits = {}
    for fn in sorted(os.listdir(SRC)):
        if not fn.endswith((".cpp", ".h")):
            continue
        path = os.path.join(SRC, fn)
        try:
            txt = open(path, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        for m in CALL_RE.finditer(txt):
            addr = m.group(1).upper().lstrip("0").zfill(8)
            hits.setdefault(addr, []).append(fn)
    return hits


def run_receipts():
    p = subprocess.run([sys.executable, os.path.join(HERE, "decomp.py"), "check"],
                       capture_output=True, text=True, cwd=ROOT)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()

    units = load_units()
    calls = scan_calls()
    missing = {addr: files for addr, files in calls.items() if addr not in units}

    rc, out = run_receipts()
    receipts_ok = (rc == 0)
    n_receipts = out.count("UNVERIFIED-EVIDENCE") + out.count("MATCH")

    report = {
        "receipts_ok": receipts_ok,
        "receipts_rc": rc,
        "receipts": n_receipts,
        "units": len(units),
        "engine_calls": len(calls),
        "calls_without_unit": len(missing),
        "missing": {k: sorted(set(v)) for k, v in sorted(missing.items())},
    }

    if a.json:
        print(json.dumps(report, indent=1))
    else:
        print(f"receipts: {'OK' if receipts_ok else 'FAIL'} ({n_receipts} receipt(s), rc={rc})")
        print(f"units: {len(units)}   engine calls found: {len(calls)}   without a unit: {len(missing)}")
        for addr, files in sorted(missing.items()):
            print(f"  MISSING UNIT 0x{addr}  called from {', '.join(sorted(set(files)))}")
        if not receipts_ok:
            print("--- decomp.py check output ---")
            print(out.strip())

    return 0 if (receipts_ok and not missing) else 1


if __name__ == "__main__":
    sys.exit(main())
