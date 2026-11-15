"""Fixed host move script for tools/two_copy.ps1 -Moves (virtual pad through tools/pilot.py; no OS input).
usage: py -3.14 two_copy_moves.py <repo> <host_pid>
Sequence: idle 3 s, run 3 s, 3 run-jumps, free-run 5 s, turn + free-run 5 s, idle, 6 taps of X, idle.
Used for docs/evidence/act-match-ab-2026-11-15.md."""
import sys, time
sys.path.insert(0, sys.argv[1] + r"\tools")
import pilot  # noqa: E402

p = pilot.Pilot(int(sys.argv[2]))
A, X = pilot.BUTTONS['A'], pilot.BUTTONS['X']


def hold(sec, **kw):
    end = time.time() + sec
    while time.time() < end:
        p.set_pad(**kw)
        time.sleep(0.05)
    p.neutral()


def mark(tag):
    pos = p.read()['pos']
    print(f"{time.strftime('%H:%M:%S')} {tag} pos=({pos[0]:.1f},{pos[1]:.1f},{pos[2]:.1f})", flush=True)


mark('start');           hold(3)
mark('run');             hold(3, ly=1.0, rt=255)
mark('jump x3')
for _ in range(3):
    hold(0.6, ly=1.0, rt=255, buttons=A); hold(0.6, ly=1.0, rt=255)
mark('freerun 5s');      hold(5, ly=1.0, rt=255, buttons=A)
mark('turn+freerun 5s'); hold(1, lx=1.0); hold(5, ly=1.0, rt=255, buttons=A)
mark('idle');            hold(2)
mark('attack x6')
for _ in range(6):
    hold(0.15, buttons=X); hold(0.45)
mark('end');             hold(2)
