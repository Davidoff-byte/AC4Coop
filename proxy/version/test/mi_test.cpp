// Offline test for the version.dll proxy. Statically imports VERSION.dll and KERNEL32!CreateMutexW like AC4BFSP.exe,
// and reproduces the guard's ownership check (CreateMutexW(NULL, FALSE, name) + WaitForSingleObject(h, 0)).
//   mi_test hold   create the mutex, own it, report, keep it for 20 s
//   mi_test probe  create the same mutex, report the WaitForSingleObject(h, 0) result and the version.dll checks
// Expected: probe beside the proxy -> wait=0 (owned); probe without the proxy (negative control) -> wait=258 (timeout).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

static void FileVersion(const char* tag, decltype(&GetFileVersionInfoSizeA) size, decltype(&GetFileVersionInfoA) info,
                        decltype(&VerQueryValueA) query)
{
    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH);
    strcat_s(path, "\\kernel32.dll");
    DWORD h = 0, n = size(path, &h);
    static BYTE buf[1 << 16];
    VS_FIXEDFILEINFO* ffi = nullptr; UINT len = 0;
    if (n && n <= sizeof(buf) && info(path, 0, n, buf) && query(buf, "\\", (LPVOID*)&ffi, &len) && ffi)
        printf("%s kernel32 version %u.%u.%u.%u (size %lu)\n", tag, HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
               HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS), n);
    else
        printf("%s version query FAILED err=%lu\n", tag, GetLastError());
}

int main(int argc, char** argv)
{
    const bool hold = argc > 1 && strcmp(argv[1], "hold") == 0;
    HANDLE h = CreateMutexW(nullptr, FALSE, L"scimitar_mutex_mitest");
    DWORD w = h ? WaitForSingleObject(h, 0) : WAIT_FAILED;
    printf("%s pid=%lu wait=%lu (%s)\n", hold ? "hold" : "probe", GetCurrentProcessId(), w,
           w == WAIT_OBJECT_0 ? "owned" : w == WAIT_TIMEOUT ? "held by another copy" : "other");

    char vpath[MAX_PATH] = "?";
    GetModuleFileNameA(GetModuleHandleA("version.dll"), vpath, MAX_PATH);
    printf("version.dll loaded from %s\n", vpath);

    // Through the import (proxy if present) vs straight from the system DLL.
    FileVersion("import:", &GetFileVersionInfoSizeA, &GetFileVersionInfoA, &VerQueryValueA);
    char sys[MAX_PATH]; GetSystemDirectoryA(sys, MAX_PATH); strcat_s(sys, "\\version.dll");
    HMODULE real = LoadLibraryA(sys);
    if (real)
        FileVersion("system:", (decltype(&GetFileVersionInfoSizeA))GetProcAddress(real, "GetFileVersionInfoSizeA"),
                    (decltype(&GetFileVersionInfoA))GetProcAddress(real, "GetFileVersionInfoA"),
                    (decltype(&VerQueryValueA))GetProcAddress(real, "VerQueryValueA"));
    fflush(stdout);
    if (hold) Sleep(20000);
    return 0;
}
