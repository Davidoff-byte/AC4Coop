"""dump_triage.py - I-705: one-screen triage of a crash dump through cdb.exe, with IDA-ready VAs.

    py -3.14 tools/dump_triage.py <file.dmp> [--cdb PATH] [--timeout SEC] [--raw out.txt]

Runs   cdb.exe -z <dmp> -c "!analyze -v; .ecxr; kb 30; q"   and extracts:
  exception code / address, faulting module + offset (FAILURE_BUCKET / MODULE_NAME / SYMBOL_NAME),
  the top frames (STACK_TEXT, else `kb 30`) with each frame's pc, and for AC4BFSP frames the IDA VA
  (pc rebased from the load base proven by raw AC4BFSP+0xOFF frames onto ImageBase 0x400000).
  cdb's `AC4BFSP!ReadData+0x..` names are nearest-export guesses (no PDB); the pc is the fact.

cdb is looked up under the Windows Kits 10 Debuggers x86 then x64 directories (the dumps are of a 32-bit
process: the x86 cdb is preferred). Nothing is installed. Exit codes:
  0 = triage printed, 2 = cdb not found / dump missing, 3 = cdb failed or timed out.
"""
from __future__ import annotations
import argparse
import os
import re
import subprocess
import sys
import time

IMAGE_BASE = 0x400000
KITS = r'C:\Program Files (x86)\Windows Kits\10\Debuggers'
CANDIDATES = [os.path.join(KITS, 'x86', 'cdb.exe'), os.path.join(KITS, 'x64', 'cdb.exe')]
KV_KEYS = ('ExceptionAddress', 'ExceptionCode', 'EXCEPTION_CODE_STR', 'FAULTING_IP', 'MODULE_NAME',
           'IMAGE_NAME', 'SYMBOL_NAME', 'FAILURE_BUCKET_ID', 'PROCESS_NAME', 'ERROR_CODE')


def find_cdb(explicit: str | None) -> str | None:
    # An explicit --cdb is the only candidate (no fallback), so a missing one is reported as absent.
    for p in ([explicit] if explicit else CANDIDATES):
        if p and os.path.isfile(p):
            return p
    return None


FRAME_RE = re.compile(r'^(?:\d+:\d+>\s*)?(?:[0-9a-f]{2}\s+)?([0-9a-f]{8})\s+([0-9a-f]{8})\s+(?:[0-9a-f]{8}\s+){3}(\S.*)$', re.I)


def frames_from(lines: list[str], start: int) -> list[tuple[int, int, str]]:
    """(childEBP, retAddr, symbol) rows starting at lines[start], stopping at the first non-row after a row."""
    rows: list[tuple[int, int, str]] = []
    for ln in lines[start:]:
        m = FRAME_RE.match(ln.strip())
        if m:
            rows.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3).strip()))
        elif rows or not ln.strip().startswith('WARNING'):
            if rows:
                break
    return rows


def parse(out: str) -> dict:
    res: dict = {'kv': {}, 'frames': [], 'source': None, 'eip': None, 'base_checks': []}
    lines = out.splitlines()
    for i, ln in enumerate(lines):
        s = ln.strip()
        for k in KV_KEYS:
            if s.startswith(k + ':') and k not in res['kv']:
                v = s[len(k) + 1:].strip()
                if not v and i + 1 < len(lines):      # FAULTING_IP: puts the value on the next line
                    v = lines[i + 1].strip()
                res['kv'][k] = v
    # Prefer !analyze's STACK_TEXT (it is already in the exception context); fall back to the last kb header.
    st = [i for i, ln in enumerate(lines) if ln.strip().startswith('STACK_TEXT:')]
    kb = [i for i, ln in enumerate(lines) if re.search(r'ChildEBP\s+RetAddr', ln)]
    rows: list = []
    if st:
        rows, res['source'] = frames_from(lines, st[0] + 1), 'STACK_TEXT'
    if not rows and kb:
        rows, res['source'] = frames_from(lines, kb[-1] + 1), 'kb 30'
    # pc of frame 0 = ExceptionAddress; pc of frame k = RetAddr of row k-1 (a return site: just after the call).
    exc = res['kv'].get('ExceptionAddress', '').split()
    pc0 = int(exc[0], 16) if exc and re.fullmatch(r'[0-9a-fA-F]+', exc[0]) else None
    for k, (_ebp, _ret, sym) in enumerate(rows):
        pc = pc0 if k == 0 else rows[k - 1][1]
        res['frames'].append((pc, sym))
        m = re.fullmatch(r'AC4BFSP(?:\.exe)?\+0x([0-9a-f]+)', sym, re.I)
        if m and pc is not None:                  # a raw module+offset frame proves the load base
            res['base_checks'].append(pc - int(m.group(1), 16))
    return res


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dmp')
    ap.add_argument('--cdb')
    ap.add_argument('--timeout', type=int, default=600)
    ap.add_argument('--raw', help='write the full cdb output here')
    a = ap.parse_args()
    for p in CANDIDATES:
        print('cdb candidate %s: %s' % (p, 'present' if os.path.isfile(p) else 'absent'))
    cdb = find_cdb(a.cdb)
    if not cdb:
        print('ERROR: cdb.exe not found (install the Windows SDK "Debugging Tools for Windows"; this tool installs nothing)')
        return 2
    if not os.path.isfile(a.dmp):
        print('ERROR: dump %s not found' % a.dmp)
        return 2
    print('using %s on %s (%.1f MB)' % (cdb, a.dmp, os.path.getsize(a.dmp) / 1e6))
    t0 = time.time()
    try:
        r = subprocess.run([cdb, '-z', a.dmp, '-c', '!analyze -v; .ecxr; kb 30; q'],
                           capture_output=True, text=True, errors='replace', timeout=a.timeout)
    except subprocess.TimeoutExpired:
        print('ERROR: cdb timed out after %d s' % a.timeout)
        return 3
    out = r.stdout + r.stderr
    print('cdb rc=%d in %.0f s, %d lines of output' % (r.returncode, time.time() - t0, out.count('\n')))
    if a.raw:
        with open(a.raw, 'w', encoding='utf-8') as f:
            f.write(out)
    res = parse(out)
    if not res['kv'] and not res['frames']:
        print('ERROR: nothing parseable in cdb output; tail:')
        print('\n'.join(out.splitlines()[-20:]))
        return 3
    print('--- exception')
    for k in KV_KEYS:
        if k in res['kv']:
            print('  %-18s %s' % (k, res['kv'][k]))
    sym = res['kv'].get('SYMBOL_NAME', '')
    if re.match(r'ac4bfsp\+', sym, re.I):
        print('  NOTE SYMBOL_NAME %s is relative to the nearest export cdb picked (e.g. ReadData), NOT the image '
              'base - do not add it to 0x%X; use the frame pcs below' % (sym, IMAGE_BASE))
    bases = sorted(set(res['base_checks']))
    print('--- AC4BFSP load base from raw AC4BFSP+0x.. frames: %s' % (
        ', '.join('0x%08X' % b for b in bases) or 'no raw frame to check'))
    rebase = bases[0] if len(bases) == 1 else IMAGE_BASE
    print('--- top frames (%s; pc = exception address for #0, else the return site in that frame)' % res['source'])
    for k, (pc, s2) in enumerate(res['frames'][:30]):
        game = s2.upper().startswith('AC4BFSP') and pc is not None
        va = ('IDA 0x%08X' % (pc - rebase + IMAGE_BASE)) if game else ''
        print('  #%02d pc=%s %-12s %s' % (k, '%08X' % pc if pc is not None else '????????', va, s2))
    if not res['frames']:
        print('  (no frame rows parsed)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
