"""probe.py - client for the [Probe] pipe (src/probe_server.h). Port of AU-RW tools/native_tap/tap_client.py.

    py -3.14 tools/probe.py STATUS
    py -3.14 tools/probe.py HOOK 0 70e810 <expectedhex> getplayer exit
    py -3.14 tools/probe.py SNAP 0 entry 1 64 ecx +50
    py -3.14 tools/probe.py EVENTS 20
    py -3.14 tools/probe.py READ 2ac1630 16
    py -3.14 tools/probe.py --pid 1234 UNLOAD

--pid defaults to the AC4BFSP.exe with the newest phase1_log in AC4_GAME_DIR (see tools/pilot.py).
`expect <hexVA> <n>` prints the n live bytes at VA, ready to paste as HOOK's expected bytes.
"""
from __future__ import annotations

import ctypes as C
import json
import sys
import time
from ctypes import wintypes as W

K = C.WinDLL('kernel32', use_last_error=True)
K.CreateFileW.argtypes = [W.LPCWSTR, W.DWORD, W.DWORD, W.LPVOID, W.DWORD, W.DWORD, W.HANDLE]
K.CreateFileW.restype = W.HANDLE
K.ReadFile.argtypes = [W.HANDLE, W.LPVOID, W.DWORD, C.POINTER(W.DWORD), W.LPVOID]
K.WriteFile.argtypes = K.ReadFile.argtypes
K.CloseHandle.argtypes = [W.HANDLE]
K.WaitNamedPipeW.argtypes = [W.LPCWSTR, W.DWORD]


def command(pid: int, text: str, timeout: float = 30.0) -> dict:
    name = '\\\\.\\pipe\\ac4probe-%d' % pid
    end = time.monotonic() + timeout
    while True:
        if not K.WaitNamedPipeW(name, 1000):
            if time.monotonic() > end:
                raise C.WinError(C.get_last_error())
            continue
        h = K.CreateFileW(name, 0xC0000000, 0, None, 3, 0, None)
        if h != W.HANDLE(-1).value:
            break
        time.sleep(0.02)
    try:
        d = W.DWORD()
        data = text.encode('ascii')
        K.WriteFile(h, data, len(data), C.byref(d), None)
        out = bytearray()
        while not out.endswith(b'\n'):
            b = C.create_string_buffer(1 << 20)
            if not K.ReadFile(h, b, len(b), C.byref(d), None) and C.get_last_error() != 234:
                raise C.WinError(C.get_last_error())
            if not d.value:
                raise RuntimeError('pipe closed')
            out.extend(b.raw[:d.value])
        K.WriteFile(h, b'ACK', 3, C.byref(d), None)
        return json.loads(out)
    finally:
        K.CloseHandle(h)


def read(pid: int, va: int, n: int) -> bytes | None:
    r = command(pid, f'READ {va:x} {n}')
    return bytes.fromhex(r['hex']) if r.get('ok') else None


def main(argv):
    pid = None
    if argv[:1] == ['--pid']:
        pid, argv = int(argv[1]), argv[2:]
    if pid is None:
        sys.path.insert(0, __file__.rsplit('\\', 1)[0].rsplit('/', 1)[0])
        from pilot import find_pid  # noqa: E402
        pid = find_pid(None)
    if argv[:1] == ['expect']:
        b = read(pid, int(argv[1], 16), int(argv[2]))
        print(b.hex() if b is not None else 'unreadable')
        return
    print(json.dumps(command(pid, ' '.join(argv)), indent=1))


if __name__ == '__main__':
    main(sys.argv[1:])
