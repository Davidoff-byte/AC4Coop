"""pilot.py - drive Edward and read his surroundings through the [Pilot] module (src/pilot.h).

Needs AC4Coop.ini [Pilot] Enabled=1 (optional KeepFocus=1, PadIndex=0). Nothing here sends OS input: the pad is
written into the game's own shared section Local\\ac4pilot-pad-<pid>, read by the patched XInputGetState import.

    py -3.14 tools/pilot.py status                       # frame, player position/matrix, counters
    py -3.14 tools/pilot.py scan [--r 40]                # characters around Edward (kind/role, fighter status)
    py -3.14 tools/pilot.py tap A [--sec 0.15]           # press and release a button
    py -3.14 tools/pilot.py stick 0 1 --sec 2 [--run]    # left stick (x right, y forward), optional RT held
    py -3.14 tools/pilot.py goto X Y [--tol 1.5] [--run] [--timeout 60]
    py -3.14 tools/pilot.py boot [--pulses 6]            # menus -> world with A presses (pair with [Coop] AutoContinue=1)
    py -3.14 tools/pilot.py mem player 0x200             # hex dump (read-only); target = player | hex address
    py -3.14 tools/pilot.py watch [--sec 30]             # position + nearest characters, once a second

--pid defaults to the AC4BFSP.exe whose phase1_log_<pid>.txt is newest in AC4_GAME_DIR (tools/paths.py).
Stick directions are camera-relative in the game, so `goto` learns the camera's yaw from Edward's own motion
(the angle between where the stick pushed and where he actually went) instead of reading the camera.
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as W
import glob
import math
import os
import struct
import sys
import time

k = ctypes.windll.kernel32
k.OpenFileMappingW.restype = W.HANDLE
k.MapViewOfFile.restype = ctypes.c_void_p
k.OpenProcess.restype = W.HANDLE

PAD_FMT = '<IIIHBBhhhh4I'            # PilotPad, 40 bytes (pack 1)
CHAR_FMT = '<III3fIiiI'              # PilotChar, 40 bytes
HDR_FMT = '<IIIII III 16f II IIII'   # PilotState header, 120 bytes
MAX_CHARS = 256
STATE_SIZE = struct.calcsize(HDR_FMT) + MAX_CHARS * struct.calcsize(CHAR_FMT)
assert struct.calcsize(PAD_FMT) == 40 and struct.calcsize(CHAR_FMT) == 40 and STATE_SIZE == 10360

BUTTONS = {'UP': 0x1, 'DOWN': 0x2, 'LEFT': 0x4, 'RIGHT': 0x8, 'START': 0x10, 'BACK': 0x20, 'LS': 0x40, 'RS': 0x80,
           'LB': 0x100, 'RB': 0x200, 'A': 0x1000, 'B': 0x2000, 'X': 0x4000, 'Y': 0x8000}
# PS3 DWARF names (leads, see bf-coop ops/evidence/guard-ai/dwarf-enums.md); PC values matched in PS3-P1-RESULTS.md
ROLE = ['All', 'Templar', 'Peasant', 'Thief', 'Courtesan', 'Mercenary', 'Target', 'CityEventBourgeois',
        'CityEventPickpocketeer', 'CityEventOfficial', 'Orator', 'Horse', 'Nurse', 'TemplarAchievement',
        'Male_Present_Hardsole', 'Assassin', 'Harasser', 'Pirate', 'Lite', 'Animal', 'Haytham', 'HomesteadNPC', 'Drunk',
        'NavalNPC', 'CartHorse', 'Criminal', 'Dealer', 'NewspaperBoy', 'Vigilantes', 'VaneMarooned', 'ElTiburon']
FSTATUS = ['Invalid', 'Active', 'Chasing', 'Blocking', 'SearchingIntercept', 'SearchingSearch', 'SearchOver',
           'LowMoraleFear', 'Update', 'OffFight', 'Incapacitated']


def game_dir():
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    try:
        import paths  # noqa
        return paths.game_dir() if hasattr(paths, 'game_dir') else os.environ.get('AC4_GAME_DIR', '')
    except Exception:
        return os.environ.get('AC4_GAME_DIR', '')


def find_pid(explicit):
    if explicit:
        return explicit
    gd = game_dir() or r"D:\SteamLibrary\steamapps\common\Assassin's Creed IV Black Flag"
    logs = glob.glob(os.path.join(gd, 'phase1_log_*.txt'))
    if not logs:
        sys.exit(f'no phase1_log_*.txt in {gd}; pass --pid')
    return int(os.path.basename(max(logs, key=os.path.getmtime))[len('phase1_log_'):-4])


class Pilot:
    def __init__(self, pid):
        self.pid = pid
        self.pad = self._map('pad', 40)
        self.state = self._map('state', STATE_SIZE)
        self.h = k.OpenProcess(0x0010 | 0x0400, False, pid)
        self.seq = 0

    def _map(self, kind, size):
        name = f'Local\\ac4pilot-{kind}-{self.pid}'
        m = k.OpenFileMappingW(0x000F001F, False, name)
        if not m:
            sys.exit(f'{name} not found: is [Pilot] Enabled=1 and is pid {self.pid} the real game?')
        p = k.MapViewOfFile(m, 0x000F001F, 0, 0, size)
        if not p:
            sys.exit(f'MapViewOfFile({name}) failed')
        return p

    # ---- state -------------------------------------------------------------------------------------------------
    def read(self):
        for _ in range(50):
            s0 = ctypes.c_uint32.from_address(self.state + 8).value
            raw = ctypes.string_at(self.state, STATE_SIZE)
            s1 = ctypes.c_uint32.from_address(self.state + 8).value
            if s0 == s1 and not (s0 & 1):
                break
            time.sleep(0.001)
        hs = struct.calcsize(HDR_FMT)
        h = struct.unpack_from(HDR_FMT, raw, 0)
        st = dict(magic=h[0], version=h[1], seq=h[2], frame=h[3], tick=h[4], world=h[5], player=h[6], human=h[7],
                  m=h[8:24], nChars=h[24], nTotal=h[25], padPolls=h[26], padServed=h[27], padIndex=h[28], focusDrops=h[29])
        chars = []
        for i in range(min(st['nChars'], MAX_CHARS)):
            c = struct.unpack_from(CHAR_FMT, raw, hs + 40 * i)
            chars.append(dict(ent=c[0], aic=c[1], rec=c[2], pos=c[3:6], w9c=c[6], status=c[7], role=c[8], flags=c[9]))
        st['chars'] = chars
        st['pos'] = st['m'][12:15]
        return st

    # ---- pad ---------------------------------------------------------------------------------------------------
    def set_pad(self, buttons=0, lx=0.0, ly=0.0, rx=0.0, ry=0.0, lt=0, rt=0, enabled=1):
        clamp = lambda v: max(-32767, min(32767, int(v * 32767)))
        self.seq += 1
        body = struct.pack('<HBBhhhh', buttons, lt, rt, clamp(lx), clamp(ly), clamp(rx), clamp(ry))
        ctypes.memmove(self.pad + 12, body, len(body))
        ctypes.c_uint32.from_address(self.pad + 4).value = enabled
        ctypes.c_uint32.from_address(self.pad + 8).value = (ctypes.c_uint32.from_address(self.pad + 8).value + 1) & 0xFFFFFFFF

    def neutral(self):
        self.set_pad(enabled=1)

    def tap(self, name, sec=0.15):
        self.set_pad(buttons=BUTTONS[name.upper()])
        time.sleep(sec)
        self.neutral()

    def rpm(self, addr, n):
        b = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
        ok = k.ReadProcessMemory(self.h, ctypes.c_void_p(addr), b, n, ctypes.byref(got))
        return b.raw[:got.value] if ok else None


def dist2(a, b):
    return (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2


def describe(c, me):
    kind, role = c['w9c'] & 7, (c['w9c'] & 0x1F8) >> 3
    rn = ROLE[role] if role < len(ROLE) else str(role)
    fs = FSTATUS[c['status']] if 0 <= c['status'] < len(FSTATUS) else ('-' if c['status'] < 0 else str(c['status']))
    d = math.sqrt(dist2(c['pos'], me))
    return (f"{d:6.1f} m  ent={c['ent']:08X} kind={kind} role={role}({rn}) fighter={fs}"
            f"{' rec=%08X' % c['rec'] if c['rec'] else ''}  pos=({c['pos'][0]:.1f},{c['pos'][1]:.1f},{c['pos'][2]:.1f})"
            f"{'  <PLAYER>' if c['flags'] & 1 else ''}")


def cmd_status(p, a):
    s = p.read()
    age = (k.GetTickCount() - s['tick']) & 0xFFFFFFFF if s['tick'] else None
    m = s['m']
    print(f"pid={p.pid} magic={s['magic']:08X} v{s['version']} frame={s['frame']} last write {age} ms ago")
    print(f"world={s['world']:08X} player={s['player']:08X} human={s['human']:08X} chars={s['nChars']}/{s['nTotal']}")
    print(f"pos=({m[12]:.2f}, {m[13]:.2f}, {m[14]:.2f})  row0=({m[0]:.2f},{m[1]:.2f},{m[2]:.2f}) "
          f"row1=({m[4]:.2f},{m[5]:.2f},{m[6]:.2f}) row2=({m[8]:.2f},{m[9]:.2f},{m[10]:.2f})")
    print(f"pad index={s['padIndex']} polls={s['padPolls']} served={s['padServed']}  focusDrops={s['focusDrops']}")


def cmd_scan(p, a):
    s = p.read()
    me = s['pos']
    rows = sorted((c for c in s['chars'] if dist2(c['pos'], me) <= a.r * a.r), key=lambda c: dist2(c['pos'], me))
    print(f"{len(rows)} of {s['nChars']} characters within {a.r} m of ({me[0]:.1f},{me[1]:.1f},{me[2]:.1f})")
    for c in rows:
        print('  ' + describe(c, me))


def cmd_tap(p, a):
    p.tap(a.button, a.sec)
    print(f'tapped {a.button} for {a.sec}s')


def cmd_stick(p, a):
    p.set_pad(lx=a.x, ly=a.y, rt=255 if a.run else 0)
    time.sleep(a.sec)
    p.neutral()
    print(f'stick ({a.x},{a.y}) for {a.sec}s, pos now {tuple(round(v, 2) for v in p.read()["pos"])}')


def wrap(x):
    return (x + math.pi) % (2 * math.pi) - math.pi


def cmd_goto(p, a):
    target = (a.x, a.y)
    offset = 0.0                    # world angle = stick angle + offset (camera yaw, learned)
    learned = False
    hist = []                       # (t, pos, commanded world angle)
    t0 = time.time()
    best = float('inf'); best_t = t0
    try:
        while time.time() - t0 < a.timeout:
            s = p.read()
            pos = s['pos']
            d = math.sqrt(dist2(pos, target))
            if d < a.tol:
                p.neutral()
                print(f'arrived: ({pos[0]:.2f},{pos[1]:.2f},{pos[2]:.2f}) d={d:.2f} m after {time.time() - t0:.1f}s '
                      f'(camera offset {math.degrees(offset):.0f} deg)')
                return 0
            if d < best - 0.3:
                best, best_t = d, time.time()
            elif time.time() - best_t > a.stuck:
                p.neutral()
                print(f'STUCK: no progress for {a.stuck}s at ({pos[0]:.2f},{pos[1]:.2f},{pos[2]:.2f}), d={d:.2f} m')
                return 2
            want = math.atan2(target[1] - pos[1], target[0] - pos[0])
            stick = want - offset
            mag = 1.0 if d > 3 else max(0.35, d / 3)
            p.set_pad(lx=math.cos(stick) * mag, ly=math.sin(stick) * mag, rt=255 if a.run and d > 4 else 0)
            now = time.time()
            hist.append((now, pos, stick))
            hist = [h for h in hist if now - h[0] < 0.6]
            if len(hist) > 4:
                t1, p1, s1 = hist[0]
                v = (pos[0] - p1[0], pos[1] - p1[1])
                if math.hypot(*v) > 0.25:
                    seen = math.atan2(v[1], v[0])
                    err = wrap(seen - (s1 + offset))
                    offset = wrap(offset + (0.8 if not learned else 0.3) * err)
                    learned = True
            time.sleep(0.05)
    finally:
        p.neutral()
    print(f'TIMEOUT after {a.timeout}s, d={d:.2f} m')
    return 1


def cmd_boot(p, a):
    """Press A until the world is up. Never moves a menu cursor (no d-pad / stick), so it only ever confirms what the
    game or [Coop] AutoContinue highlighted: the single-player tile, then the save slot."""
    last = p.read()['frame']
    for i in range(a.pulses):
        time.sleep(a.wait)
        s = p.read()
        if s['frame'] != last and s['nTotal'] >= 2:
            print(f'in world: frame {s["frame"]}, {s["nTotal"]} characters, pos {tuple(round(v, 1) for v in s["pos"])}')
            return 0
        last = s['frame']
        p.tap('A', 0.15)
        print(f'pulse {i + 1}: A (frame {s["frame"]}, chars {s["nTotal"]})', flush=True)
    for _ in range(int(a.settle / 1)):
        time.sleep(1)
        s = p.read()
        if s['frame'] != last and s['nTotal'] >= 2:
            print(f'in world: frame {s["frame"]}, {s["nTotal"]} characters, pos {tuple(round(v, 1) for v in s["pos"])}')
            return 0
        last = s['frame']
    print('not in world yet')
    return 1


def cmd_mem(p, a):
    s = p.read()
    base = s['player'] if a.target == 'player' else int(a.target, 16)
    n = int(a.length, 0)
    data = p.rpm(base, n)
    if data is None:
        sys.exit(f'unreadable at {base:08X}')
    for o in range(0, len(data), 16):
        row = data[o:o + 16]
        f = ' '.join(f'{x:9.3f}' if abs(x) < 1e6 and x == x else '        .' for x in struct.unpack('<4f', row.ljust(16, b'\0')))
        print(f'{base + o:08X} +{o:04X}  {row.hex(" ")}  | {f}')


def cmd_watch(p, a):
    end = time.time() + a.sec
    while time.time() < end:
        s = p.read()
        me = s['pos']
        near = sorted((c for c in s['chars'] if not c['flags'] & 1), key=lambda c: dist2(c['pos'], me))[:3]
        print(f"frame {s['frame']} pos=({me[0]:.1f},{me[1]:.1f},{me[2]:.1f}) chars={s['nChars']} | " +
              ' ; '.join(describe(c, me).split('  pos=')[0] for c in near), flush=True)
        time.sleep(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--pid', type=int)
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('status')
    x = sub.add_parser('scan'); x.add_argument('--r', type=float, default=40)
    x = sub.add_parser('tap'); x.add_argument('button'); x.add_argument('--sec', type=float, default=0.15)
    x = sub.add_parser('stick'); x.add_argument('x', type=float); x.add_argument('y', type=float)
    x.add_argument('--sec', type=float, default=1.0); x.add_argument('--run', action='store_true')
    x = sub.add_parser('goto'); x.add_argument('x', type=float); x.add_argument('y', type=float)
    x.add_argument('--tol', type=float, default=1.5); x.add_argument('--run', action='store_true')
    x.add_argument('--timeout', type=float, default=60); x.add_argument('--stuck', type=float, default=4)
    x = sub.add_parser('boot'); x.add_argument('--pulses', type=int, default=6); x.add_argument('--wait', type=float, default=5)
    x.add_argument('--settle', type=float, default=60)
    x = sub.add_parser('mem'); x.add_argument('target'); x.add_argument('length', nargs='?', default='0x100')
    x = sub.add_parser('watch'); x.add_argument('--sec', type=float, default=30)
    a = ap.parse_args()
    p = Pilot(find_pid(a.pid))
    fn = globals()['cmd_' + a.cmd]
    sys.exit(fn(p, a) or 0)


if __name__ == '__main__':
    main()
