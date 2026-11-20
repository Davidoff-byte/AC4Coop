"""minidump.py - write a minidump (thread stacks + handle data: MiniDumpNormal | MiniDumpWithHandleData) of a live process.

    py -I tools/minidump.py <pid> <out.dmp>

Used by tools/two_copy.ps1 to capture a hung game copy. Read-only on the target. Dumps are never committed.
"""
import ctypes as C
import sys
from ctypes import wintypes as W

pid, out = int(sys.argv[1]), sys.argv[2]
k = C.WinDLL('kernel32', use_last_error=True)
d = C.WinDLL('dbghelp', use_last_error=True)
k.OpenProcess.restype = W.HANDLE
k.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
k.CreateFileW.restype = W.HANDLE
k.CreateFileW.argtypes = [W.LPCWSTR, W.DWORD, W.DWORD, C.c_void_p, W.DWORD, W.DWORD, W.HANDLE]
k.CloseHandle.argtypes = [W.HANDLE]
d.MiniDumpWriteDump.argtypes = [W.HANDLE, W.DWORD, W.HANDLE, C.c_int, C.c_void_p, C.c_void_p, C.c_void_p]

PROCESS_QUERY_INFORMATION, PROCESS_VM_READ, PROCESS_DUP_HANDLE = 0x0400, 0x0010, 0x0040
h = k.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, False, pid)
if not h:
    sys.exit('OpenProcess(%d) failed err=%d' % (pid, C.get_last_error()))
f = k.CreateFileW(out, 0x40000000, 0, None, 2, 0x80, None)   # GENERIC_WRITE, CREATE_ALWAYS
if f in (None, W.HANDLE(-1).value):
    k.CloseHandle(h)
    sys.exit('CreateFileW(%s) failed err=%d' % (out, C.get_last_error()))
ok = d.MiniDumpWriteDump(h, pid, f, 0x0 | 0x4, None, None, None)
err = C.get_last_error() & 0xFFFFFFFF
k.CloseHandle(f)
k.CloseHandle(h)

# A 64-bit dbghelp cannot dump a 32-bit (WOW64) target: MiniDumpWriteDump returns
# ERROR_PARTIAL_COPY (0x12B) and writes an empty file. Fall back to the WOW64 copy of comsvcs.dll,
# which runs inside the target's own bitness. Measured 2026-11-20: a hung 32-bit AC4BFSP.exe produced
# a 0-byte dump and err=0x8007012B through the path above.
if not ok and err == 0x12B:
    import os
    import subprocess
    syswow = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'SysWOW64')
    rundll = os.path.join(syswow, 'rundll32.exe')
    comsvcs = os.path.join(syswow, 'comsvcs.dll')
    if os.path.isfile(rundll) and os.path.isfile(comsvcs):
        for p in (out, out):          # comsvcs creates the file itself; make sure it is not stale
            try:
                os.remove(p)
                break
            except OSError:
                pass
        subprocess.run([rundll, comsvcs + ',', 'MiniDump', str(pid), out, 'full'],
                       capture_output=True, timeout=120)
        if os.path.isfile(out) and os.path.getsize(out) > 0:
            print('dump ok (wow64 comsvcs): %s (%d B)' % (out, os.path.getsize(out)))
            sys.exit(0)
        print('dump failed err=0x%08X and the WOW64 comsvcs fallback produced nothing' % err)
        sys.exit(1)
    print('dump failed err=0x%08X (no WOW64 comsvcs available)' % err)
    sys.exit(1)

print('dump ok: %s' % out if ok else 'dump failed err=0x%08X' % err)
sys.exit(0 if ok else 1)
