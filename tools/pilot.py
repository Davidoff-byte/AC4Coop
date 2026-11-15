"""pilot.py - drive Edward and read his surroundings through the [Pilot] module (src/pilot.h).

Needs AC4Coop.ini [Pilot] Enabled=1 (optional KeepFocus=1, PadIndex=0). Nothing here sends OS input: the pad is
written into the game's own shared section Local\\ac4pilot-pad-<pid>, read by the patched XInputGetState import.

    py -3.14 tools/pilot.py status                       # frame, player position/matrix, counters
    py -3.14 tools/pilot.py scan [--r 40]                # characters around Edward (kind/role, fighter status)
    py -3.14 tools/pilot.py tap A [--sec 0.15]           # press and release a button
    py -3.14 tools/pilot.py stick 0 1 --sec 2 [--run]    # left stick (x right, y forward), optional RT held
    py -3.14 tools/pilot.py goto X Y [--tol 1.5] [--speed 1] [--timeout 60] [--route] [--stick]
                                  # in-process world-space intent (IHumanGround slots); --stick = old camera-relative pad
    py -3.14 tools/pilot.py intent DX DY [--speed 1] [--sec 1]   # world-space push, renewed until --sec elapses
    py -3.14 tools/pilot.py hg --can 75 --do 76 --arg h:ENT   # S2 probe: HumanGround interface slot calls (src/pilot.cpp HgCall)
    py -3.14 tools/pilot.py route X Y                    # plan only (navmesh straight-line checks, no screen)
    py -3.14 tools/pilot.py boot [--pulses 6]            # menus -> world with A presses (pair with [Coop] AutoContinue=1)
    py -3.14 tools/pilot.py mem player 0x200             # hex dump (read-only); target = player | hex address
    py -3.14 tools/pilot.py watch [--sec 30]             # position + nearest characters, once a second
    py -3.14 tools/pilot.py teleport X Y Z [--yaw DEG]   # game thread, Entity::SetGlobalMatrixInternal
    py -3.14 tools/pilot.py fps [N]                      # show / set the frame cap (0 = off)
    py -3.14 tools/pilot.py ray X Y Z X2 Y2 Z2 [--layer 11]   # engine ray cast (hit pos, normal, distance, type)
    py -3.14 tools/pilot.py ground [X Y Z]               # ground height under a point (default: Edward)
    py -3.14 tools/pilot.py nav [X Y Z] | navline X Y [X2 Y2]  # navmesh triangle / straight-line walkability
    py -3.14 tools/pilot.py edges [--r 3] [--zmin -1] [--zmax 4]   # climbable edges (guidance) around Edward
    py -3.14 tools/pilot.py sense [--range 8] [--h 1] [--rays 16]  # ray fan around Edward: what blocks each way

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
CTL_FMT = '<IIIIi8fIIII8I'          # PilotCtl header, 100 bytes; then u32 rcount, 3 pad, float res[256]
CTL_SIZE = 1140
MAX_CHARS = 256
STATE_SIZE = struct.calcsize(HDR_FMT) + MAX_CHARS * struct.calcsize(CHAR_FMT)
assert struct.calcsize(CTL_FMT) == 100 and struct.calcsize(PAD_FMT) == 40 and struct.calcsize(CHAR_FMT) == 40 and STATE_SIZE == 10360

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
        self.ctl = self._map('ctl', CTL_SIZE)
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

    # ---- control ---------------------------------------------------------------------------------------------------
    def ctl_read(self):
        c = struct.unpack_from(CTL_FMT, ctypes.string_at(self.ctl, 100))
        return dict(magic=c[0], req=c[1], done=c[2], cmd=c[3], status=c[4], f=c[5:13], fpsCap=c[13], presents=c[14], fps=c[15],
                    presentPatched=c[16])

    def ctl_send(self, cmd, floats, timeout=5.0):
        """Fill cmd/f, bump req, wait for done == req (the game runs it on the player frame or the poll thread)."""
        fl = list(floats) + [float('nan')] * (8 - len(floats))
        ctypes.memmove(self.ctl + 12, struct.pack('<Ii8f', cmd, 0, *fl), 40)
        req = (ctypes.c_uint32.from_address(self.ctl + 4).value + 1) & 0xFFFFFFFF
        ctypes.c_uint32.from_address(self.ctl + 4).value = req
        end = time.time() + timeout
        while time.time() < end:
            if ctypes.c_uint32.from_address(self.ctl + 8).value == req:
                return ctypes.c_int32.from_address(self.ctl + 16).value
            time.sleep(0.02)
        return None

    def ctl_send_u32(self, cmd, words, timeout=5.0):
        """ctl_send with raw u32 words in f[] (kCmdHgCall reads the bits, not floats)."""
        w = list(words) + [0] * (8 - len(words))
        ctypes.memmove(self.ctl + 12, struct.pack('<Ii8I', cmd, 0, *w), 40)
        req = (ctypes.c_uint32.from_address(self.ctl + 4).value + 1) & 0xFFFFFFFF
        ctypes.c_uint32.from_address(self.ctl + 4).value = req
        end = time.time() + timeout
        while time.time() < end:
            if ctypes.c_uint32.from_address(self.ctl + 8).value == req:
                return ctypes.c_int32.from_address(self.ctl + 16).value
            time.sleep(0.02)
        return None

    def results(self, n, stride):
        cnt = ctypes.c_uint32.from_address(self.ctl + 100).value
        raw = ctypes.string_at(self.ctl + 116, 4 * 256)
        vals = struct.unpack('<256f', raw)
        return cnt, [vals[i * stride:(i + 1) * stride] for i in range(min(cnt, 256 // stride))]

    def results_u32(self):
        return struct.unpack('<256I', ctypes.string_at(self.ctl + 116, 4 * 256))

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


def nav_clear(p, a, b, z):
    """Engine navmesh straight-line check (sub_47EA80): True when walkable in a straight line on HumanGround."""
    st = p.ctl_send(6, [a[0], a[1], z, b[0], b[1], z])
    return st == 0 and (p.results_u32()[0] & 0xFF) == 0      # byte return (al); low byte also right on build c0daf16


def plan_route(p, start, goal, z, step=3.0, margin=9.0, budget=600):
    """A* on a step-metre lattice around start/goal; an edge exists when the engine's navmesh straight-line check
    (sub_47EA80) is clear. Edges are tested lazily and cached; `budget` caps the number of checks. The path is then
    shortened by skipping to the farthest waypoint with a clear straight line. Returns waypoints (goal last) or None."""
    import heapq
    if nav_clear(p, start, goal, z):
        return [goal]
    ox, oy = start
    def pt(n):
        return (ox + n[0] * step, oy + n[1] * step)
    gx, gy = round((goal[0] - ox) / step), round((goal[1] - oy) / step)
    lo_x = min(0, gx) - int(margin / step); hi_x = max(0, gx) + int(margin / step)
    lo_y = min(0, gy) - int(margin / step); hi_y = max(0, gy) + int(margin / step)
    cache, checks = {}, [0]
    def edge(a, b):
        key = (a, b) if a < b else (b, a)
        if key not in cache:
            if checks[0] >= budget:
                return False
            checks[0] += 1
            cache[key] = nav_clear(p, pt(a), pt(b), z)
        return cache[key]
    h = lambda n: math.dist(pt(n), goal)
    s0 = (0, 0)
    openq = [(h(s0), 0.0, s0)]
    came, gbest = {s0: None}, {s0: 0.0}
    found = None
    while openq:
        f, g, n = heapq.heappop(openq)
        if g > gbest.get(n, 1e9) + 1e-6:
            continue
        if math.dist(pt(n), goal) < step * 1.5 and nav_clear(p, pt(n), goal, z):
            found = n; break
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                if not dx and not dy:
                    continue
                m = (n[0] + dx, n[1] + dy)
                if not (lo_x <= m[0] <= hi_x and lo_y <= m[1] <= hi_y):
                    continue
                ng = g + step * math.hypot(dx, dy)
                if ng >= gbest.get(m, 1e9) or not edge(n, m):
                    continue
                gbest[m] = ng; came[m] = n
                heapq.heappush(openq, (ng + h(m), ng, m))
        if checks[0] >= budget:
            break
    if found is None:
        return None
    path = []
    n = found
    while n is not None:
        path.append(pt(n)); n = came[n]
    path = path[::-1] + [goal]
    out, i = [], 0                                       # string pulling: farthest clear waypoint from each point
    while i < len(path) - 1:
        j = len(path) - 1
        while j > i + 1 and not nav_clear(p, path[i], path[j], z):
            j -= 1
        out.append(path[j]); i = j
    return out


def plan_route_retry(p, start, goal, z):
    """The lattice is anchored at the start; an awkward start (next to a stall) can wall it in. Retry finer and wider
    (live 2026-11-15: NO ROUTE from (77.4,-75.5), a 6-leg route from (76.3,-74.3) to the same goal)."""
    for step, margin, budget in ((3.0, 9.0, 600), (2.0, 14.0, 900), (1.5, 18.0, 1200)):
        r = plan_route(p, start, goal, z, step=step, margin=margin, budget=budget)
        if r is not None:
            return r
    return None


def cmd_route(p, a):
    s = p.read(); pos = s['pos']
    t0 = time.time()
    r = plan_route_retry(p, (pos[0], pos[1]), (a.x, a.y), pos[2])
    print(f'planned in {time.time() - t0:.1f}s from ({pos[0]:.1f},{pos[1]:.1f}): ' +
          ('NO ROUTE' if r is None else ' -> '.join(f'({w[0]:.1f},{w[1]:.1f})' for w in r)))
    return 0 if r else 1


def goto_inproc(p, target, tol, a):
    """Steered by the game every player frame (kCmdGoto); this only starts it and waits for arrived/stuck."""
    st = p.ctl_send(10, [target[0], target[1], a.speed, tol, a.stuck])
    if st != 0:
        print(f'goto refused: status {st} (-4 = intent hooks not installed)'); return 4
    t0 = time.time()
    while time.time() - t0 < a.timeout:
        time.sleep(0.05)
        c = p.ctl_read()
        state = struct.unpack_from('<I', ctypes.string_at(p.ctl + 68, 4))[0]
        if state in (2, 3, 4):
            pos = p.read()['pos']
            d = struct.unpack_from('<f', ctypes.string_at(p.ctl + 72, 4))[0]
            subs = struct.unpack_from('<II', ctypes.string_at(p.ctl + 76, 8))
            name = {2: 'arrived', 3: 'STUCK', 4: 'stopped'}[state]
            print(f'{name}: ({pos[0]:.2f},{pos[1]:.2f},{pos[2]:.2f}) d={d:.2f} m after {time.time() - t0:.1f}s '
                  f'(substitutions dir={subs[0]} speed={subs[1]})')
            return 0 if state == 2 else 2
    p.ctl_send(11, [])
    print(f'TIMEOUT after {a.timeout}s'); return 1


def leg(p, target, tol, a):
    return drive_to(p, target, tol, a) if a.stick else goto_inproc(p, target, tol, a)


def cmd_climb(p, a):
    """Climbing intent (Climbing interface slots 37/38, src/pilot.cpp): 2D input x sideways / y up, held for --sec."""
    before = p.read()['pos']
    end = time.time() + a.sec
    while time.time() < end:
        st = p.ctl_send(12, [a.x, a.y, a.speed, 300])
        if st != 0:
            print(f'climb intent refused: status {st}'); return 4
        time.sleep(0.1)
    p.ctl_send(11, [])
    time.sleep(0.4)
    after = p.read()['pos']
    subs = struct.unpack_from('<III', ctypes.string_at(p.ctl + 88, 12))
    print(f'from ({before[0]:.2f},{before[1]:.2f},{before[2]:.2f}) to ({after[0]:.2f},{after[1]:.2f},{after[2]:.2f}); '
          f'substitutions slot37={subs[0]} slot38={subs[1]} hooks={subs[2]}')
    return 0


def cmd_hg(p, a):
    """S2 live probe (kCmdHgCall 13, src/pilot.cpp HgCall): call a HumanGround interface slot on Edward from his own update.
    --iface 0 IHumanGround (HG+0x10) | 1 HG+0x14 | 2 HG+0x70 | 3 HG+0x74; --can/--do slot numbers (Do runs only if the Can
    returned non-zero, or with no --can); --arg N (int, 0x.. ok) or --arg h:ENTITY (passes &copy of *(ENTITY+0xC8)).
    Path/context changes for the next 120 frames go to the phase1 log as [HG] watch lines."""
    NONE = 0xFFFFFFFF
    args, mask = [], 0
    for i, s in enumerate(a.arg or []):
        if s.startswith('h:'):
            mask |= 1 << i
            s = s[2:]
        args.append(int(s, 0) & 0xFFFFFFFF)
    if len(args) > 4:
        print('at most 4 args'); return 2
    can = NONE if a.can is None else a.can
    do = NONE if a.do is None else a.do
    iface = a.iface | (0x100 if a.puppet else 0)   # 0x100: run on the co-op puppet's own update (Pilot::OnPuppetFrame)
    st = p.ctl_send_u32(13, [iface, can, do, len(args) | (mask << 8)] + args)
    r = struct.unpack('<11I', ctypes.string_at(p.ctl + 116, 44))
    path = '/'.join('-' if v == NONE else str(v) for v in r[5:11])
    print(f'status {st} | can {"-" if not r[2] else f"{r[0]:#x}"} do {"-" if not r[3] else f"{r[1]:#x}"} | before ctx={r[4] if r[4] < 0x80000000 else r[4] - 2**32} path={path}')
    return 0 if st == 0 else 3


def cmd_intent(p, a):
    before = p.read()['pos']
    end = time.time() + a.sec
    while time.time() < end:
        st = p.ctl_send(9, [a.dx, a.dy, a.speed, 300])
        if st != 0:
            print(f'intent refused: status {st}'); return 4
        time.sleep(0.1)
    p.ctl_send(11, [])
    time.sleep(0.3)
    after = p.read()['pos']
    v = (after[0] - before[0], after[1] - before[1])
    ang = math.degrees(math.atan2(v[1], v[0])); want = math.degrees(math.atan2(a.dy, a.dx))
    subs = struct.unpack_from('<II', ctypes.string_at(p.ctl + 76, 8))
    print(f'moved {math.hypot(*v):.2f} m at {ang:.0f} deg (asked {want:.0f} deg) from ({before[0]:.2f},{before[1]:.2f}) to '
          f'({after[0]:.2f},{after[1]:.2f}); substitutions dir={subs[0]} speed={subs[1]}')


def cmd_goto(p, a):
    if a.route:
        pos = p.read()['pos']
        r = plan_route_retry(p, (pos[0], pos[1]), (a.x, a.y), pos[2])
        if r is None:
            print('NO ROUTE on the navmesh'); return 3
        print('route: ' + ' -> '.join(f'({w[0]:.1f},{w[1]:.1f})' for w in r))
        for i, w in enumerate(r):
            last = i == len(r) - 1
            rc = leg(p, w, a.tol if last else max(a.tol, 1.5), a)
            if rc:
                return rc
        return 0
    return leg(p, (a.x, a.y), a.tol, a)


DRIVE = {'offset': 0.0, 'learned': False}   # camera yaw estimate shared across legs of one goto


def drive_to(p, target, tol, a):
    offset = DRIVE['offset']        # world angle = stick angle + offset (camera yaw, learned)
    learned = DRIVE['learned']
    hist = []                       # (t, pos, commanded world angle)
    t0 = time.time()
    best = float('inf'); best_t = t0
    try:
        while time.time() - t0 < a.timeout:
            s = p.read()
            pos = s['pos']
            d = math.sqrt(dist2(pos, target))
            if d < tol:
                DRIVE['offset'], DRIVE['learned'] = offset, learned
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
    if a.fps:
        st = p.ctl_send(2, [a.fps])
        print(f'frame cap {a.fps}: status {st}')
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


def cmd_teleport(p, a):
    before = p.read()['pos']
    yaw = math.radians(a.yaw) if a.yaw is not None else float('nan')
    st = p.ctl_send(1, [a.x, a.y, a.z, yaw])
    time.sleep(0.5)
    after = p.read()['pos']
    time.sleep(1.5)
    later = p.read()['pos']
    print(f'teleport status={st} (0 ok, -1 no player, -2 exception, None = not executed: in a menu?)')
    print(f'  before ({before[0]:.2f},{before[1]:.2f},{before[2]:.2f}) -> 0.5 s ({after[0]:.2f},{after[1]:.2f},{after[2]:.2f})'
          f' -> 2 s ({later[0]:.2f},{later[1]:.2f},{later[2]:.2f}); target ({a.x:.2f},{a.y:.2f},{a.z:.2f})')
    held = math.sqrt(dist2(later, (a.x, a.y))) < 1.0
    print('  RESULT: ' + ('held at the target' if held else 'did NOT hold (snapped back or moved)'))
    return 0 if held else 1


def cmd_fps(p, a):
    if a.n is not None:
        st = p.ctl_send(2, [a.n])
        print(f'set cap {a.n}: status {st} (-4 = Present not patched)')
        time.sleep(2.2)
    c = p.ctl_read()
    print(f"cap={c['fpsCap']} measured={c['fps']} fps presents={c['presents']} presentPatched={c['presentPatched']}")


CTYPE = {1: 'ground', 2: 'slope', 4: 'wall', 8: 'ceiling'}
GSUB = {1: 'LedgeGrab', 2: 'Beam', 3: 'Ladder', 4: 'Pole', 5: 'Rope', 6: 'Surface', 7: 'Kiosk', 12: 'HayStack',
        13: 'LedgeGrabHandsOnly', 16: 'Parallel', 17: 'HorizontalParallel', 18: 'LedgeGrabPole', 19: 'PassOver',
        21: 'HorizontalRope', 22: 'LedgeLookDown'}   # PS3 DWARF GuidanceObjectSubType (lead)


def here(p, a):
    pos = p.read()['pos']
    return [a.x if a.x is not None else pos[0], a.y if a.y is not None else pos[1], a.z if a.z is not None else pos[2]]


def cmd_ray(p, a):
    st = p.ctl_send(3, [a.x, a.y, a.z, a.x2, a.y2, a.z2, a.layer])
    n, rows = p.results(1, 8)
    if st != 0 or not n:
        print(f'status {st}: no hit'); return 0 if st == 0 else 1
    r = rows[0]
    print(f'status {st}: HIT pos=({r[0]:.2f},{r[1]:.2f},{r[2]:.2f}) normal=({r[3]:.2f},{r[4]:.2f},{r[5]:.2f}) '
          f'dist={r[6]:.2f} m type={int(r[7])}({CTYPE.get(int(r[7]), "?")})')


def cmd_ground(p, a):
    pt = here(p, a)
    st = p.ctl_send(4, pt + [a.half])
    n, rows = p.results(1, 1)
    print(f'status {st}: ' + (f'ground z={rows[0][0]:.3f} under ({pt[0]:.2f},{pt[1]:.2f}) (query z {pt[2]:.2f})' if n else 'no ground hit'))


def cmd_nav(p, a):
    pt = here(p, a)
    st = p.ctl_send(5, pt)
    tri = p.results_u32()[0]
    n, rows = p.results(1, 2)
    print(f'status {st}: ' + (f'navmesh triangle {tri:08X}, walk z={rows[0][1]:.3f} at ({pt[0]:.2f},{pt[1]:.2f})' if n else
                               f'no HumanGround triangle at ({pt[0]:.2f},{pt[1]:.2f},{pt[2]:.2f})'))


def cmd_navline(p, a):
    pos = p.read()['pos']
    ax, ay = (a.x, a.y) if a.x2 is not None else (pos[0], pos[1])
    bx, by = (a.x2, a.y2) if a.x2 is not None else (a.x, a.y)
    st = p.ctl_send(6, [ax, ay, pos[2], bx, by, pos[2]])
    u = p.results_u32()
    code = u[0] & 0xFF                                 # byte return (al), see pilot_world.cpp
    print(f'status {st}: ({ax:.1f},{ay:.1f}) -> ({bx:.1f},{by:.1f}) ' + ('CLEAR' if code == 0 else f'BLOCKED code={code} err={u[1]} {u[2]} {u[3]}'))


def cmd_edges(p, a):
    st = p.ctl_send(7, [a.r, a.zmin, a.zmax, float(a.mask)])
    n, rows = p.results(32, 8)
    me = p.read()['pos']
    tot = int(rows[0][7]) if rows else 0
    print(f'status {st}: {n} nearest of {tot} climbable edges in +/-{a.r} m, z {a.zmin}..{a.zmax} around ({me[0]:.1f},{me[1]:.1f},{me[2]:.1f})')
    for r in sorted(rows, key=lambda r: min(dist2(r[0:3], me), dist2(r[3:6], me))):
        sub = int(r[6])
        print(f'  {GSUB.get(sub, sub)!s:18} A=({r[0]:.2f},{r[1]:.2f},{r[2]:.2f}) B=({r[3]:.2f},{r[4]:.2f},{r[5]:.2f}) '
              f'height {min(r[2], r[5]) - me[2]:+.2f}..{max(r[2], r[5]) - me[2]:+.2f} m')


def cmd_sense(p, a):
    st = p.ctl_send(8, [a.range, a.h, a.layer, a.rays])
    n, rows = p.results(32, 4)
    print(f'status {st}: {n} rays, range {a.range} m at +{a.h} m (0 deg = +x, counter-clockwise)')
    for i, r in enumerate(rows):
        deg = 360.0 * i / n
        what = 'open' if r[1] == 0 else f'{CTYPE.get(int(r[1]), int(r[1]))} at {r[0]:.2f} m'
        print(f'  {deg:5.1f} deg  {"#" * int(round(10 * r[0] / a.range)):<10} {what}')


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
    x.add_argument('--route', action='store_true', help='plan waypoints on the navmesh first')
    x.add_argument('--stick', action='store_true', help='old camera-relative virtual-pad controller')
    x.add_argument('--speed', type=float, default=1.0)
    x = sub.add_parser('intent'); x.add_argument('dx', type=float); x.add_argument('dy', type=float)
    x.add_argument('--speed', type=float, default=1.0); x.add_argument('--sec', type=float, default=1.0)
    x = sub.add_parser('climb'); x.add_argument('x', type=float); x.add_argument('y', type=float)
    x.add_argument('--speed', type=float, default=1.0); x.add_argument('--sec', type=float, default=1.0)
    x = sub.add_parser('hg'); x.add_argument('--iface', type=int, default=0); x.add_argument('--can', type=int)
    x.add_argument('--do', type=int); x.add_argument('--arg', action='append'); x.add_argument('--puppet', action='store_true')
    x = sub.add_parser('route'); x.add_argument('x', type=float); x.add_argument('y', type=float)
    x = sub.add_parser('boot'); x.add_argument('--pulses', type=int, default=6); x.add_argument('--wait', type=float, default=5)
    x.add_argument('--settle', type=float, default=60); x.add_argument('--fps', type=float, default=0)   # 0: no Present patch (cap preceded 4 lost copies in 7 two-copy runs, docs/evidence/s0-two-copy-rig-2026-11-15.md)
    x = sub.add_parser('teleport'); x.add_argument('x', type=float); x.add_argument('y', type=float); x.add_argument('z', type=float)
    x.add_argument('--yaw', type=float)
    x = sub.add_parser('fps'); x.add_argument('n', type=float, nargs='?')
    x = sub.add_parser('ray')
    for n in ('x', 'y', 'z', 'x2', 'y2', 'z2'): x.add_argument(n, type=float)
    x.add_argument('--layer', type=float, default=11)
    for name in ('ground', 'nav'):
        x = sub.add_parser(name)
        for n in ('x', 'y', 'z'): x.add_argument(n, type=float, nargs='?')
        if name == 'ground': x.add_argument('--half', type=float, default=3.0)
    x = sub.add_parser('navline')
    for n in ('x', 'y', 'x2', 'y2'): x.add_argument(n, type=float, nargs='?')
    x = sub.add_parser('edges'); x.add_argument('--r', type=float, default=3); x.add_argument('--zmin', type=float, default=-1)
    x.add_argument('--zmax', type=float, default=4); x.add_argument('--mask', type=lambda v: int(v, 0), default=0)
    x = sub.add_parser('sense'); x.add_argument('--range', type=float, default=8); x.add_argument('--h', type=float, default=1.0)
    x.add_argument('--layer', type=float, default=11); x.add_argument('--rays', type=float, default=16)
    x = sub.add_parser('mem'); x.add_argument('target'); x.add_argument('length', nargs='?', default='0x100')
    x = sub.add_parser('watch'); x.add_argument('--sec', type=float, default=30)
    a = ap.parse_args()
    p = Pilot(find_pid(a.pid))
    fn = globals()['cmd_' + a.cmd]
    sys.exit(fn(p, a) or 0)


if __name__ == '__main__':
    main()
