"""provenance_gate.py - I-703: refuse to score a run whose build cannot be reproduced.

    py -3.14 tools/provenance_gate.py <phase1_log_*.txt> [...] [--asi PATH] [--allow-other-asi] [--repo DIR]

Every phase1_log_<pid>.txt starts with the plugin's provenance line (src/dllmain.cpp, task 0.2):

    [Build] asi=<64 hex|unavailable> commit=<short> tree=<clean|dirty>

FAIL (exit 1) when, for any [Build] line in any given log:
  * the line is missing, or is `[Build] unavailable`, or asi=unavailable
  * tree=dirty                              (the binary came from uncommitted source)
  * commit is not an ancestor of HEAD       (`git merge-base --is-ancestor <commit> HEAD` in --repo)
  * asi sha256 != sha256(--asi)             (default bin/AC4Coop.asi) unless --allow-other-asi
PASS (exit 0) otherwise. Exit 2 = usage / unreadable file / --asi missing.
Stdlib only, read-only.
"""
from __future__ import annotations
import argparse
import hashlib
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD_RE = re.compile(r'\[Build\]\s+(?:asi=(?P<asi>\S+)\s+commit=(?P<commit>\S+)\s+tree=(?P<tree>\S+)|(?P<unavail>unavailable))')


def sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def is_ancestor(repo: str, commit: str) -> tuple[bool, str]:
    r = subprocess.run(['git', '-C', repo, 'merge-base', '--is-ancestor', commit, 'HEAD'],
                       capture_output=True, text=True)
    if r.returncode == 0:
        return True, 'ancestor of HEAD'
    if r.returncode == 1:
        return False, 'not an ancestor of HEAD'
    return False, 'git could not resolve commit (%s)' % (r.stderr.strip() or r.returncode)


def check_log(path: str, repo: str, asi_hash: str | None, allow_other: bool) -> list[str]:
    fails: list[str] = []
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        lines = [(i + 1, ln.rstrip('\n')) for i, ln in enumerate(f) if '[Build]' in ln]
    if not lines:
        return ['%s: no [Build] line' % path]
    for no, ln in lines:
        m = BUILD_RE.search(ln)
        where = '%s:%d' % (os.path.basename(path), no)
        if not m or m.group('unavail'):
            fails.append('%s: unusable provenance `%s`' % (where, ln.strip()))
            continue
        asi, commit, tree = m.group('asi'), m.group('commit'), m.group('tree')
        print('%s: asi=%s commit=%s tree=%s' % (where, asi, commit, tree))
        if tree != 'clean':
            fails.append('%s: tree=%s (binary built from uncommitted source)' % (where, tree))
        ok, why = is_ancestor(repo, commit)
        if not ok:
            fails.append('%s: commit %s %s' % (where, commit, why))
        if asi == 'unavailable':
            fails.append('%s: asi=unavailable' % where)
        elif not allow_other and asi_hash is not None and asi.lower() != asi_hash:
            fails.append('%s: asi %s != local build %s' % (where, asi, asi_hash))
    return fails


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--asi', default=os.path.join(ROOT, 'bin', 'AC4Coop.asi'))
    ap.add_argument('--allow-other-asi', action='store_true')
    ap.add_argument('--repo', default=ROOT)
    a = ap.parse_args()
    asi_hash = None
    if not a.allow_other_asi:
        if not os.path.isfile(a.asi):
            print('ERROR: --asi %s not found (build first, or pass --allow-other-asi)' % a.asi)
            return 2
        asi_hash = sha256(a.asi)
        print('local asi %s sha256=%s' % (a.asi, asi_hash))
    fails: list[str] = []
    for p in a.logs:
        if not os.path.isfile(p):
            print('ERROR: %s not found' % p)
            return 2
        fails += check_log(p, a.repo, asi_hash, a.allow_other_asi)
    for f in fails:
        print('FAIL ' + f)
    print('provenance: %s (%d log(s), %d failure(s))' % ('FAIL' if fails else 'PASS', len(a.logs), len(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
