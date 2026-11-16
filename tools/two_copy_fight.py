"""two_copy_fight.py - scripted real fight for a two-copy run (tools/two_copy.ps1 -Interactive): one copy walks to a soldier
group, engages the nearest soldier and fights it with the virtual pad. Combat-breadth test of
docs/evidence/combat-breadth-2026-11-16.md.

    py -3.14 tools/two_copy_fight.py <pid> [--x 30 --y -75] [--sec 40] [--counter]

Steps: goto (x, y) running; slot EnterFight (kCmdHgCall 13, CanEnterFight 75 / DoEnterFight 76 with &[soldier+0xC8]) on the
nearest Templar-role character; then every 0.6 s an X attack aimed at the current target (stick toward it), and with
--counter a B tap on every 4th beat (the counter button). Prints each engaged target and a final line.
Pad input only goes through the pilot shared memory of that pid (no OS input)."""
import argparse
import math
import sys
import time
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pilot  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument('pid', type=int)
ap.add_argument('--x', type=float, default=30.0)
ap.add_argument('--y', type=float, default=-75.0)
ap.add_argument('--sec', type=float, default=40.0)
ap.add_argument('--counter', action='store_true')
ap.add_argument('--mode', choices=['mixed', 'counter-only', 'none'], default='mixed',
                help='counter-only: B only after engaging (a counter is then the only way to hurt a soldier); none: no buttons')
ap.add_argument('--provoke', type=int, default=4, help='counter-only: X strikes first, to start the fight (0 = none)')
ap.add_argument('--no-goto', action='store_true', help='fight where the player stands (no walk, no slot EnterFight)')
a = ap.parse_args()

p = pilot.Pilot(a.pid)
X, B = pilot.BUTTONS['X'], pilot.BUTTONS['B']


def soldiers(r=12.0):
    s = p.read()
    me = s['pos']
    out = []
    for c in s['chars']:
        if c['flags'] & 1:
            continue
        role = (c['w9c'] & 0x1F8) >> 3
        if c['w9c'] & 7 != 2 or role != 1:   # kind 2 NPC, role 1 Templar (soldiers)
            continue
        d = math.sqrt(pilot.dist2(c['pos'], me))
        if d <= r:
            out.append((d, c))
    out.sort(key=lambda t: t[0])
    return me, out


def hold(sec, **kw):
    end = time.time() + sec
    while time.time() < end:
        p.set_pad(**kw)
        time.sleep(0.03)
    p.neutral()


subprocess_goto = [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pilot.py'), '--pid', str(a.pid),
                   'goto', str(a.x), str(a.y), '--run', '--timeout', '60']
import subprocess  # noqa: E402
if not a.no_goto:
    print(subprocess.run(subprocess_goto, capture_output=True, text=True).stdout.strip().splitlines()[-1:], flush=True)
    me, near = soldiers(15.0)
    if not near:
        print('no soldier within 15 m', flush=True)
        sys.exit(2)
    d, tgt = near[0]
    st = p.ctl_send_u32(13, [0, 75, 76, 1 | (1 << 8), tgt['ent']])
    print(f'engage ent={tgt["ent"]:08X} d={d:.1f} m slot EnterFight status {st} soldiers in 15 m: {len(near)}', flush=True)
    time.sleep(0.3)

t0 = time.time(); beat = 0; last_t = None
while time.time() - t0 < a.sec:
    me, near = soldiers(10.0)
    if near:
        d, c = near[0]
        if last_t != c['ent']:
            print(f'{time.time() - t0:5.1f}s target ent={c["ent"]:08X} d={d:.1f}', flush=True)
            last_t = c['ent']
        # world-space stick toward the target is camera-relative in the game; keep the stick neutral and let the fight
        # system's target lock aim the attack (as real_fight.py did)
    beat += 1
    if a.mode == 'none':
        time.sleep(0.6)
        continue
    if a.mode == 'counter-only':
        # 4 strikes start the fight (guards only engage after being hit); a strike is ~4 of 36 life (111 permille), so a
        # lethal request after these can only be a counter kill
        if beat <= a.provoke:
            hold(0.12, buttons=X); time.sleep(0.48)
            if beat == a.provoke: print(f'{time.time() - t0:5.1f}s counter-only from here', flush=True)
            continue
        hold(0.12, buttons=B)
        time.sleep(0.18)
        continue
    if a.counter and beat % 4 == 0:
        hold(0.12, buttons=B)
    else:
        hold(0.12, buttons=X)
    time.sleep(0.48)
print(f'fight done after {beat} beats', flush=True)
