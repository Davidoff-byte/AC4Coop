"""Host move script for guard-placement runs (tools/two_copy.ps1 -Moves; docs/design/enemy-placement.md section 6).
usage: py -3.14 placement_moves.py <repo> <host_pid>
About 110 s around the start point, so guards stay in scope and the [Place] / [Bind] 5 s windows fill up:
idle 15 s, 8 x (walk 3 s, turn 0.7 s), idle 12 s, 6 x (sprint 2.5 s, turn 0.7 s), idle 12 s, run back 6 s, idle 10 s.
Virtual pad through tools/pilot.py; no OS input."""
import sys, time
sys.path.insert(0, sys.argv[1] + r"\tools")
import pilot  # noqa: E402

p = pilot.Pilot(int(sys.argv[2]))


def hold(sec, **kw):
    end = time.time() + sec
    while time.time() < end:
        p.set_pad(**kw)
        time.sleep(0.05)
    p.neutral()


def mark(tag):
    pos = p.read()['pos']
    print(f"{time.strftime('%H:%M:%S')} {tag} pos=({pos[0]:.1f},{pos[1]:.1f},{pos[2]:.1f})", flush=True)


mark('start');  hold(15)
mark('walk loop')
for _ in range(8):
    hold(3, ly=0.6); hold(0.7, lx=1.0)
mark('idle');   hold(12)
mark('sprint loop')
for _ in range(6):
    hold(2.5, ly=1.0, rt=255); hold(0.7, lx=-1.0)
mark('idle');   hold(12)
mark('run back'); hold(0.8, lx=1.0); hold(6, ly=1.0)
mark('idle');   hold(10)
mark('end')
