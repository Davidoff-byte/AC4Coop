#!/usr/bin/env python3
"""net_selftest.py - run the plugin's wire selftests (src/net.cpp SelfTestWire) without the game.

SelfTestWire runs inside Net::Init, i.e. only in a game with [Net] Enabled=1. This tool compiles src/net.cpp
with AC4COOP_NET_SELFTEST_MAIN (a console main() at the end of net.cpp that calls SelfTestWire) plus the units
it links against, with the same x86 v143 toolset as the ASI, into bin/net_selftest/ (ignored), and runs it.
It covers the entity codec (roundtrip, resend, nochange, deaddelta, resurrect, halfdead, anim, stale) and the
proto 15 intention lane (golden, parse, handshake, deliver, ack, dedup, reorder, loss30, rejoin).

  py tools\\net_selftest.py          build + run; prints the selftest lines; exit 0 = every test PASS

Nothing here touches the game folder or the ASI. Exit 0 pass, 1 fail, 2 build error.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "bin" / "net_selftest"
VCVARS = [
    Path(r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"),
    Path(r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"),
    Path(r"C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars32.bat"),
    Path(r"C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars32.bat"),
]
# net.cpp and what it calls (paths, lobby code, UPnP, STUN). No engine code: the selftest never touches the game.
SOURCES = ["src/net.cpp", "src/paths.cpp", "src/lobby_code.cpp", "src/upnp.cpp", "src/stun.cpp"]
LIBS = ["ws2_32.lib", "winhttp.lib", "iphlpapi.lib", "user32.lib", "bcrypt.lib"]


def main() -> int:
    vc = next((p for p in VCVARS if p.exists()), None)
    if vc is None:
        print("[net_selftest] vcvars32.bat not found (VS 2022 Build Tools x86 toolset needed)")
        return 2
    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / "net_selftest.exe"
    cl = (f'cl /nologo /EHsc /O2 /MT /W3 /DWIN32 /DNDEBUG /DAC4COOP_NET_SELFTEST_MAIN /Isrc '
          + " ".join(SOURCES) + f' /Fo"{OUT}\\\\" /Fe"{exe}" /link ' + " ".join(LIBS))
    r = subprocess.run(f'call "{vc}" >nul && {cl}', shell=True, cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0 or not exe.exists():
        print(r.stdout[-4000:], r.stderr[-2000:])
        print("[net_selftest] RESULT build=FAIL")
        return 2
    warn = [ln for ln in r.stdout.splitlines() if ": warning " in ln]
    for ln in warn:
        print(ln)
    r = subprocess.run([str(exe)], cwd=OUT, capture_output=True, text=True, timeout=120)
    lines = [ln for ln in r.stdout.splitlines() if "selftest" in ln]
    for ln in lines:
        print(ln)
    ok = r.returncode == 0 and any("SelfTestWire PASS" in ln for ln in lines) and not any("FAIL" in ln for ln in lines)
    print(f"[net_selftest] RESULT {'PASS' if ok else 'FAIL'} (exit={r.returncode}, compiler warnings={len(warn)})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
