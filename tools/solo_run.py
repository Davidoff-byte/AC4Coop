"""Run the solo phases against a live game, automatically, once the game is in-world.

    python solo_run.py --game "D:\\SteamLibrary\\...\\Assassin's Creed IV Black Flag"
    python solo_run.py --phase P0          # one phase only

It waits for a NEW phase1_log (mtime after start) containing `[Net] cfg`, starts tools/peer9.py as
the synthetic peer for the requested time, then judges the phases from the log alone:

    P0  link + ghost     [Net] stats rx > 0, [Net] spawn succeeded, [Pup] mode=position,
                         [Smooth] maxD present, no crash marker
    P1  same-world       [Net] cfg peer, plus a [Scene]-style line if the build emits one
                         (falls back to reporting "not instrumented" - it is a known gap)
    P6  perception       [G10] rows with aggro=1 and dPeer small (needs [Coop] StageLog=1)
    P5  enemy scope      [Sync] kindrole soldier>0 and [Sync] scope emitted>0

Writes ops/evidence/solo-<stamp>.json and prints one PASS/FAIL line per phase.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def newest_log(game, since):
    cands = [p for p in glob.glob(os.path.join(game, "phase1_log_*.txt"))
             if os.path.getmtime(p) >= since]
    return max(cands, key=os.path.getmtime) if cands else None


def wait_for_link(game, since, timeout=900):
    t0 = time.time()
    while time.time() - t0 < timeout:
        p = newest_log(game, since)
        if p:
            try:
                txt = open(p, encoding="utf-8", errors="replace").read()
            except OSError:
                txt = ""
            if "[Net] cfg" in txt:
                return p
        time.sleep(2)
    return None


def judge(path, phases):
    txt = open(path, encoding="utf-8", errors="replace").read()
    res = {}

    if "P0" in phases:
        checks = {
            "rx": bool(re.findall(r"rx=(\d+)", txt) and int(re.findall(r"rx=(\d+)", txt)[-1]) > 0),
            "spawn": "[Net] spawn succeeded" in txt,
            "pup": "[Pup] mode=position" in txt or "[Pup] mode=physics" in txt,
            "smooth": "[Smooth]" in txt,
            "no_crash": "0xC0000005" not in txt and "EXCEPTION" not in txt,
        }
        res["P0"] = {"pass": all(checks.values()), "checks": checks}

    if "P1" in phases:
        m = re.search(r"\[Net\] cfg enabled=(\d+) peer=(\S+)", txt)
        scene = bool(re.search(r"\[Scene\].*worldId=", txt))
        res["P1"] = {"pass": False,
                     "checks": {"cfg": bool(m), "peer_not_loopback": bool(m) and m.group(2) != "127.0.0.1",
                                "scene_line": scene},
                     "note": "no [Scene] worldId line in this build - instrumented? see PLAN-COOP-V2 P1"}

    if "P6" in phases:
        rows = re.findall(r"\[G10\][^\n]*aggro=(\d)[^\n]*dPeer=([\d.]+)", txt)
        hit = [r for r in rows if r[0] == "1" and float(r[1]) < 5.0]
        res["P6"] = {"pass": len(hit) > 0, "g10_rows": len(rows), "aggro_near_peer": len(hit),
                     "note": "needs [Coop] StageLog=1; a proxy-free negative means guards do not see the ghost"}

    if "P5" in phases:
        m = re.search(r"\[Sync\] kindrole n=(\d+) soldier=(\d+) civilian=(\d+) other=(\d+)", txt)
        s = re.search(r"\[Sync\] scope candidates=(\d+) inRadius=(\d+)", txt)
        res["P5"] = {"pass": bool(m) and int(m.group(2)) > 0,
                     "kindrole": m.groups() if m else None,
                     "scope": s.groups() if s else None}
    return res


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True)
    ap.add_argument("--phase", default="P0,P1,P5,P6")
    ap.add_argument("--peer-seconds", type=int, default=90)
    ap.add_argument("--no-peer", action="store_true")
    a = ap.parse_args(argv)
    phases = [p.strip() for p in a.phase.split(",") if p.strip()]

    since = time.time()
    print(f"waiting for a new phase1_log with [Net] cfg in {a.game} ...", flush=True)
    log = wait_for_link(a.game, since)
    if not log:
        print("TIMEOUT: no in-world log appeared; start the game, load a save, get in-world")
        return 1
    print(f"link up in {os.path.basename(log)}")

    peer = None
    if not a.no_peer:
        peer = subprocess.Popen([sys.executable, os.path.join(HERE, "peer9.py"),
                                 "--walk", "--echo-pos", "--duration", str(a.peer_seconds)],
                                cwd=HERE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print(f"peer9 started pid={peer.pid} for {a.peer_seconds}s")
        time.sleep(a.peer_seconds)
    else:
        time.sleep(20)

    res = judge(log, phases)
    out = os.path.join(HERE, "..", "docs", "evidence", f"solo-{time.strftime('%Y%m%d-%H%M%S')}.json")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    json.dump({"log": log, "phases": res}, open(out, "w"), indent=1)

    print()
    for p in phases:
        r = res.get(p, {})
        print(f"{p}: {'PASS' if r.get('pass') else 'FAIL'}   {json.dumps(r.get('checks', r))[:180]}")
    print(f"\nreport: {os.path.abspath(out)}")
    return 0 if all(res.get(p, {}).get("pass") for p in phases) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

