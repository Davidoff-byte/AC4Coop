"""spine_gate.py - the hard gate for the decompilation spine.

Three rules, all machine-checked, exit 0 only when all hold:

  1. RECEIPTS RE-DERIVE.  Every receipt in decomp/receipts/ is re-checked by
     `decomp.py check` against the pinned binary. A drifted candidate, a missing
     evidence file or a hand-edited verdict fails.
  2. NO UNDOCUMENTED ENGINE CALL.  Every direct call into the game made from the
     plugin (`))0xADDR)(` cast-and-call form) must have a unit in
     decomp/units.json. If we call an engine function, it gets a receipt - that is
     the rule that keeps "I called it and it seemed fine" out of the codebase.
  3. NO UNDOCUMENTED ENGINE CALL, ADDRESS IN ARGUMENT POSITION.  The same rule for
     the two call shapes rule 2 is blind to:

       (a) `Hook::CallClassMethod<...>(0xADDR, ...)` - and CallCdeclMethod /
           CallStdMethod / CallFastCallMethod - where the callee address is the
           template helper's FIRST ARGUMENT. It never takes the `))0xADDR)(` shape,
           so rule 2 cannot see it: that is how ConstructGhostBody called
           0x00C4F2A0 and ResolveEntityByIdRaw called 0x00A1F160 with no unit.
           The argument list may wrap onto the next line, so the match spans lines.
       (b) `))0xADDR)(` with a NINE-hex-digit literal, which rule 2's {6,8} bound
           cannot match. A 9-digit literal is an engine address with a stray leading
           zero (`0x004DD9824` is `0x04DD9824`); the source should spell it with 8
           digits, but if one survives the gate must still catch the call.

     Addresses from every rule are normalised to 8 hex digits before lookup, so
     `0x9F0B50` and `0x009F0B50` are the same unit. A literal that is not a unit is
     reported, never skipped: the gate does not decide which literals "look like"
     engine addresses.

Not covered (deliberate, and checked by hand instead): a call whose address reaches
the call site through a variable, e.g. `ShipSpawnReqFn fn = (ShipSpawnReqFn)
(base() + 0x00C82810); ... fn(shipEnt, 0)` in phase1.cpp. No textual rule can follow
that, and a rule that only caught the `(base() + 0xRVA)(...)` shape would give false
confidence about the variable form.

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

# The same shape with nine hex digits: `))0x19FB4C0_)(args)`. CALL_RE's {6,8} bound
# cannot match these, and a 9-digit literal is a real engine address (`0x004DD9824`).
CALL9_RE = re.compile(r"\)\s*0x([0-9A-Fa-f]{9})\s*\)\s*\(")

# `Hook::CallClassMethod<void, void*, void*>(0xADDR, ...)` - callee is the helper's
# first argument, optionally cast, and the argument list may wrap onto the next line.
# `[^;{}]*?` keeps the template-argument list one expression: template arguments
# cannot contain ';' or '{'.
CALL_HELPER_RE = re.compile(
    r"\bCall[A-Za-z0-9_]*Method\s*<[^;{}]*?>\s*\(\s*(?:\([^()]*\)\s*)?0x([0-9A-Fa-f]{6,9})\b")


def load_units():
    with open(UNITS, encoding="utf-8") as f:
        d = json.load(f)
    return {k.upper().lstrip("0").zfill(8): v for k, v in d.get("units", {}).items()}


def norm(lit):
    return lit.upper().lstrip("0").zfill(8)


def scan_calls(rule):
    hits = {}
    for fn in sorted(os.listdir(SRC)):
        if not fn.endswith((".cpp", ".h")):
            continue
        path = os.path.join(SRC, fn)
        try:
            txt = open(path, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        for m in rule.finditer(txt):
            addr = norm(m.group(1))
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
    calls = scan_calls(CALL_RE)          # rule 2: cast-and-call, 6-8 digits (unchanged)
    calls9 = scan_calls(CALL9_RE)        # rule 3(b): cast-and-call, 9 digits
    calls_t = scan_calls(CALL_HELPER_RE)  # rule 3(a): Call*Method<...>(0xADDR, ...)

    # rule 3 = (a) and (b) merged; the same address found by both is one entry.
    arg_calls = {}
    for src in (calls9, calls_t):
        for addr, files in src.items():
            arg_calls.setdefault(addr, []).extend(files)

    missing = {addr: files for addr, files in calls.items() if addr not in units}
    missing_arg = {addr: files for addr, files in arg_calls.items() if addr not in units}

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
        "argument_position_calls": len(arg_calls),
        "argument_position_without_unit": len(missing_arg),
        "argument_position_missing": {k: sorted(set(v)) for k, v in sorted(missing_arg.items())},
    }

    if a.json:
        print(json.dumps(report, indent=1))
    else:
        print(f"receipts: {'OK' if receipts_ok else 'FAIL'} ({n_receipts} receipt(s), rc={rc})")
        print(f"units: {len(units)}   engine calls found: {len(calls)}   without a unit: {len(missing)}")
        for addr, files in sorted(missing.items()):
            print(f"  MISSING UNIT 0x{addr}  called from {', '.join(sorted(set(files)))}"
                  f"  [rule: ))0xADDR)(]")
        print(f"          address-in-argument calls found: {len(arg_calls)}"
              f"   without a unit: {len(missing_arg)}")
        for addr, files in sorted(missing_arg.items()):
            print(f"  MISSING UNIT 0x{addr}  called from {', '.join(sorted(set(files)))}"
                  f"  [rule: Call*Method<...> first argument / 9-digit cast-and-call]")
        if not receipts_ok:
            print("--- decomp.py check output ---")
            print(out.strip())

    return 0 if (receipts_ok and not missing and not missing_arg) else 1


if __name__ == "__main__":
    sys.exit(main())
