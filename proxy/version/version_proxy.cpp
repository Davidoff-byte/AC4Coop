// version.dll proxy: lifts AC4's single-instance guard on every start, independent of AC4Coop.asi,
// the ASI loader, the launcher arm and every ini flag.
//
// Why version.dll: AC4BFSP.exe (sha256 bc11ad79...) statically imports VERSION.dll
// (GetFileVersionInfoA, GetFileVersionInfoSizeA, VerQueryValueA; pefile, 2026-11-15) and VERSION.dll is not
// in KnownDLLs on this machine, so the copy in the game folder is loaded before the exe's entry point.
//
// The guard (docs/evidence/single-instance-guard-2026-11-15.md, IDA): _WinMain@16 0x00406D25
// CreateMutexW(NULL, FALSE, L"scimitar_mutex_ac4bf_61979"), then WaitForSingleObject(h, 0) and a 5 s wait;
// if the mutex cannot be owned, WinMain returns 0. CreateMutexW's only caller in the exe is WinMain.
// Fix: IAT-hook the main module's KERNEL32!CreateMutexW and give names starting with "scimitar_mutex_" a
// per-process suffix, so every copy owns its own mutex. Other names pass through unchanged.
//
// The exports forward to the real %SystemRoot%\System32\version.dll (SysWOW64 for this x86 process),
// loaded on first use, so this file never calls LoadLibrary under the loader lock.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winver.h>
#include <stdio.h>
#include <stdlib.h>   // _countof
#include <wchar.h>

// ---------------------------------------------------------------- log ------------------------------------------------
static wchar_t g_logPath[MAX_PATH];

static void Log(const char* fmt, ...)
{
    char line[512];
    va_list ap; va_start(ap, fmt);
    int n = _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)strlen(line);
    OutputDebugStringA(line);
    if (!g_logPath[0]) return;
    HANDLE f = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0; WriteFile(f, line, (DWORD)n, &w, nullptr);
    CloseHandle(f);
}

// ---------------------------------------------------------------- mutex rename ---------------------------------------
typedef HANDLE (WINAPI *CreateMutexW_t)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
static CreateMutexW_t g_realCreateMutexW = nullptr;
static const wchar_t kGuardPrefix[] = L"scimitar_mutex_";

static HANDLE WINAPI MyCreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCWSTR name)
{
    wchar_t buf[300];
    if (name && wcsncmp(name, kGuardPrefix, _countof(kGuardPrefix) - 1) == 0 && wcslen(name) < 200) {
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%s_mi%lu", name, GetCurrentProcessId());
        Log("[MI] pid=%lu CreateMutexW \"%S\" -> \"%S\"\r\n", GetCurrentProcessId(), name, buf);
        name = buf;
    }
    return g_realCreateMutexW(sa, owner, name);
}

// Find the main module's IAT slot for KERNEL32!CreateMutexW by name, via the import name table
// (OriginalFirstThunk), then swap it.
static void HookMainModuleCreateMutexW()
{
    BYTE* base = (BYTE*)GetModuleHandleW(nullptr);
    FARPROC real = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateMutexW");
    __try {
        auto* dos = (IMAGE_DOS_HEADER*)base;
        auto* nt  = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) { Log("[MI] pid=%lu no import directory\r\n", GetCurrentProcessId()); return; }
        for (auto* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; ++d) {
            if (_stricmp((const char*)(base + d->Name), "KERNEL32.dll") != 0) continue;
            if (!d->OriginalFirstThunk) { Log("[MI] pid=%lu KERNEL32 has no name table\r\n", GetCurrentProcessId()); return; }
            auto* names = (IMAGE_THUNK_DATA*)(base + d->OriginalFirstThunk);
            auto* slots = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
            for (size_t i = 0; names[i].u1.AddressOfData; ++i) {
                if (IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) continue;
                auto* ibn = (IMAGE_IMPORT_BY_NAME*)(base + names[i].u1.AddressOfData);
                if (strcmp((const char*)ibn->Name, "CreateMutexW") != 0) continue;
                void** slot = (void**)&slots[i].u1.Function;
                // slot_ok=0 would mean the loader has not filled the IAT yet (it would overwrite us later).
                Log("[MI] pid=%lu base=%p slot=%p (rva=%08X) *slot=%p real=%p slot_ok=%d\r\n",
                    GetCurrentProcessId(), base, slot, (unsigned)((BYTE*)slot - base), *slot, real,
                    *slot == (void*)real ? 1 : 0);
                if (*slot == (void*)&MyCreateMutexW) return;
                g_realCreateMutexW = (CreateMutexW_t)*slot;
                DWORD old = 0;
                if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
                    Log("[MI] pid=%lu VirtualProtect failed err=%lu\r\n", GetCurrentProcessId(), GetLastError());
                    return;
                }
                *slot = (void*)&MyCreateMutexW;
                VirtualProtect(slot, sizeof(void*), old, &old);
                Log("[MI] pid=%lu hooked=%d\r\n", GetCurrentProcessId(), *slot == (void*)&MyCreateMutexW ? 1 : 0);
                return;
            }
        }
        Log("[MI] pid=%lu main module does not import KERNEL32!CreateMutexW\r\n", GetCurrentProcessId());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[MI] pid=%lu import walk faulted\r\n", GetCurrentProcessId());
    }
}

// ---------------------------------------------------------------- forwarding to the real version.dll -----------------
static HMODULE RealVersion()
{
    static HMODULE s_mod = nullptr;
    HMODULE m = (HMODULE)InterlockedCompareExchangePointer((PVOID*)&s_mod, nullptr, nullptr);
    if (m) return m;
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);   // x86 process on x64 Windows: redirected to SysWOW64
    if (!n || n > MAX_PATH - 14) return nullptr;
    wcscat_s(path, L"\\version.dll");
    m = LoadLibraryW(path);
    if (m && InterlockedCompareExchangePointer((PVOID*)&s_mod, m, nullptr) != nullptr) {
        FreeLibrary(m);   // another thread won; drop our extra reference
        m = s_mod;
    }
    return m;
}

template <class F> static F Real(const char* name)
{
    HMODULE m = RealVersion();
    return m ? (F)GetProcAddress(m, name) : nullptr;
}

// Each wrapper takes its exact signature from the SDK declaration (decltype), so nothing is hand-typed.
#define FWD(ret, fail, name, params, args)                                                     \
    extern "C" ret WINAPI Px_##name params                                                      \
    {                                                                                           \
        static decltype(&::name) s_fn = nullptr;                                                \
        if (!s_fn) s_fn = Real<decltype(&::name)>(#name);                                       \
        if (!s_fn) { SetLastError(ERROR_PROC_NOT_FOUND); return fail; }                         \
        return s_fn args;                                                                       \
    }

FWD(BOOL,  FALSE, GetFileVersionInfoA,
    (LPCSTR f, DWORD h, DWORD len, LPVOID data), (f, h, len, data))
FWD(BOOL,  FALSE, GetFileVersionInfoW,
    (LPCWSTR f, DWORD h, DWORD len, LPVOID data), (f, h, len, data))
FWD(BOOL,  FALSE, GetFileVersionInfoExA,
    (DWORD fl, LPCSTR f, DWORD h, DWORD len, LPVOID data), (fl, f, h, len, data))
FWD(BOOL,  FALSE, GetFileVersionInfoExW,
    (DWORD fl, LPCWSTR f, DWORD h, DWORD len, LPVOID data), (fl, f, h, len, data))
FWD(DWORD, 0,     GetFileVersionInfoSizeA,
    (LPCSTR f, LPDWORD h), (f, h))
FWD(DWORD, 0,     GetFileVersionInfoSizeW,
    (LPCWSTR f, LPDWORD h), (f, h))
FWD(DWORD, 0,     GetFileVersionInfoSizeExA,
    (DWORD fl, LPCSTR f, LPDWORD h), (fl, f, h))
FWD(DWORD, 0,     GetFileVersionInfoSizeExW,
    (DWORD fl, LPCWSTR f, LPDWORD h), (fl, f, h))
FWD(DWORD, 0,     VerFindFileA,
    (DWORD fl, LPCSTR fn, LPCSTR win, LPCSTR app, LPSTR cur, PUINT curLen, LPSTR dst, PUINT dstLen),
    (fl, fn, win, app, cur, curLen, dst, dstLen))
FWD(DWORD, 0,     VerFindFileW,
    (DWORD fl, LPCWSTR fn, LPCWSTR win, LPCWSTR app, LPWSTR cur, PUINT curLen, LPWSTR dst, PUINT dstLen),
    (fl, fn, win, app, cur, curLen, dst, dstLen))
FWD(DWORD, 0,     VerInstallFileA,
    (DWORD fl, LPCSTR src, LPCSTR dst, LPCSTR srcDir, LPCSTR dstDir, LPCSTR curDir, LPSTR tmp, PUINT tmpLen),
    (fl, src, dst, srcDir, dstDir, curDir, tmp, tmpLen))
FWD(DWORD, 0,     VerInstallFileW,
    (DWORD fl, LPCWSTR src, LPCWSTR dst, LPCWSTR srcDir, LPCWSTR dstDir, LPCWSTR curDir, LPWSTR tmp, PUINT tmpLen),
    (fl, src, dst, srcDir, dstDir, curDir, tmp, tmpLen))
FWD(BOOL,  FALSE, VerQueryValueA,
    (LPCVOID blk, LPCSTR sub, LPVOID* buf, PUINT len), (blk, sub, buf, len))
FWD(BOOL,  FALSE, VerQueryValueW,
    (LPCVOID blk, LPCWSTR sub, LPVOID* buf, PUINT len), (blk, sub, buf, len))

// Undeclared in the SDK; the real version.dll is a bare jmp to this API-set export (see version_proxy.def).
#pragma comment(linker, "/EXPORT:GetFileVersionInfoByHandle=api-ms-win-core-version-private-l1-1-0.GetFileVersionInfoByHandle,@2")

// ---------------------------------------------------------------- entry ----------------------------------------------
BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        DWORD n = GetModuleFileNameW(mod, g_logPath, MAX_PATH);
        wchar_t* slash = (n && n < MAX_PATH) ? wcsrchr(g_logPath, L'\\') : nullptr;
        if (slash && (size_t)(slash - g_logPath) + 22 < MAX_PATH) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - g_logPath), L"AC4MultiInstance.log");
        else g_logPath[0] = 0;
        HookMainModuleCreateMutexW();
    }
    return TRUE;
}
