"""ship_coop_run.py - the scripted two-copy run behind docs/evidence/ship-coop-2026-11-16.md (one-off).

    py -3.14 tools/oneoff/ship_coop_run.py HOST_PID GUEST_PID GAME_DIR

Both players must already be aboard their (co-located) Jackdaws and the host at the wheel (ctx 25).
Phases: A host sails + turns, guest walks / stands on deck; B guest takes the wheel (teleported to the wheel stand in
its own ship's frame, then holds B) and steers; C host leaves and retakes the wheel and steers; D coast.
Markers go to stdout with QPC-free wall times; the CSVs carry the measurements.
"""
import os, subprocess, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
import pilot

HOST, GUEST, GAME = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
START = sys.argv[4] if len(sys.argv) > 4 else 'A'
WHEEL_STAND = (0.65, -12.95, 5.6)   # deck frame of the spot where holding B took the wheel (host, 115.0,-86.8 world)
h, g = pilot.Pilot(HOST), pilot.Pilot(GUEST)
T0 = time.time()


def mark(s):
    print(f"[{time.time() - T0:7.1f}s] {s}", flush=True)


def hold(p, sec, lx=0.0, ly=0.0, btn='', rt=0):
    b = 0
    for n in btn.split('+'):
        if n:
            b |= pilot.BUTTONS[n]
    p.set_pad(buttons=b, lx=lx, ly=ly, rt=rt)
    time.sleep(sec)
    p.neutral()


def last_f(pid):
    rows = [l for l in open(os.path.join(GAME, f'ship_coop_{pid}.csv'), encoding='utf-8', errors='replace') if l.startswith('F,') and l.endswith(chr(10)) and l.count(',') >= 23]
    return rows[-1].split(',')


def ctx(pid):
    return int(last_f(pid)[6])


def ship_matrix(pid):
    """Ship translation + row0 from the newest F row (row1 = row0 rotated +90 deg about Z, the deck is level)."""
    f = last_f(pid)
    return float(f[8]), float(f[9]), float(f[10]), float(f[11]), float(f[12])


def to_world(pid, l):
    tx, ty, tz, r0x, r0y = ship_matrix(pid)
    r1x, r1y = -r0y, r0x
    return tx + l[0] * r0x + l[1] * r1x, ty + l[0] * r0y + l[1] * r1y, tz + l[2]


def take_wheel(pid, p, who):
    for attempt in range(4):
        x, y, z = to_world(pid, WHEEL_STAND)
        subprocess.run([sys.executable, os.path.join(os.path.dirname(__file__), '..', 'pilot.py'), '--pid', str(pid),
                        'teleport', f'{x:.2f}', f'{y:.2f}', f'{z:.2f}', '--yaw', '0'], capture_output=True)
        time.sleep(1.0)
        hold(p, 1.0, btn='B')
        time.sleep(1.5)
        if ctx(pid) == 25:
            mark(f"{who} AT THE WHEEL (attempt {attempt + 1})")
            return True
    mark(f"{who} could NOT take the wheel")
    return False


mark(f"start host ctx={ctx(HOST)} guest ctx={ctx(GUEST)}")
# ---- A: host raises sail and turns; guest walks and stands on deck ----
if START == "A":
    mark("A: host raises sail")
    # at the wheel A = IncreaseSpeed, B = DecreaseSpeed / LeaveWheel (BF-RW NavalControlScheme digest, observed: B left the wheel)
    hold(h, 0.2, btn='A'); time.sleep(1.0); hold(h, 0.2, btn='A'); time.sleep(4.0)
for i, (lx, ly, sec, still) in (enumerate([(0, 1, 3, 6), (1, 0, 2, 6), (0, -1, 3, 6), (-1, 0, 2, 8)]) if START == "A" else []):
    if i == 1:
        mark("A: host turns starboard"); h.set_pad(lx=0.6)
    if i == 3:
        mark("A: host straightens"); h.neutral()
    mark(f"A: guest walks ({lx},{ly}) {sec}s then stands {still}s")
    hold(g, sec, lx=lx, ly=ly); time.sleep(still)
h.neutral()
# ---- B: guest takes the wheel and steers ----
mark("B: guest goes to the wheel")
if take_wheel(GUEST, g, 'guest'):
    time.sleep(3)
    mark("B: guest steers port 8 s")
    hold(g, 8, lx=-0.6)
    time.sleep(6)
# ---- C: host leaves and retakes the wheel ----
mark("C: host leaves the wheel (B)")
hold(h, 0.3, btn='B'); time.sleep(2.0)
mark(f"C: host ctx now {ctx(HOST)}")
if take_wheel(HOST, h, 'host'):
    time.sleep(3)
    mark("C: host steers starboard 8 s")
    hold(h, 8, lx=0.6)
# ---- D: coast ----
mark("D: coast 10 s"); time.sleep(10)
mark(f"end host ctx={ctx(HOST)} guest ctx={ctx(GUEST)}")
