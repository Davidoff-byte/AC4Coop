// Boot-video hook. See video_hook.h.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <intrin.h>
#include <cstdio>
#include <cstring>
#include "video_hook.h"
#include "paths.h"

#pragma intrinsic(_ReturnAddress)

namespace {

typedef void* (__stdcall* BinkOpen_t)(const char* name, unsigned flags);
BinkOpen_t g_realBinkOpen = nullptr;
int        g_log = 0;
LONG       g_count = 0;

ULONGLONG ProcessAgeMs() {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    FILETIME now; GetSystemTimeAsFileTime(&now);
    const ULONGLONG a = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
    const ULONGLONG b = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
    return b > a ? (b - a) / 10000 : 0;
}

void Log(const char* fmt, ...) {
    char line[700];
    va_list ap; va_start(ap, fmt); _vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
    line[sizeof(line) - 1] = 0;
    FILE* f = fopen(Paths::LogPath("video_log.txt"), "a");
    if (f) { fputs(line, f); fclose(f); }
    OutputDebugStringA(line);
}

void* __stdcall MyBinkOpen(const char* name, unsigned flags) {
    void* ret = _ReturnAddress();
    const LONG n = InterlockedIncrement(&g_count);
    void* h = g_realBinkOpen(name, flags);
    if (g_log)
        Log("[Video] #%ld t=%llums open \"%s\" flags=0x%08x caller=%p (rva %08x) -> %p\n", n, ProcessAgeMs(),
            name ? name : "(null)", flags, ret, (unsigned)((uintptr_t)ret - (uintptr_t)GetModuleHandleW(nullptr)), h);
    return h;
}

// The IAT slot of `func` imported by name from `dll` in the main module (import descriptors + hint/name table).
void** FindImportSlot(const char* dll, const char* func) {
    const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    __try {
        auto* dos = (IMAGE_DOS_HEADER*)base;
        auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        const IMAGE_DATA_DIRECTORY& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + d.VirtualAddress); imp->Name; ++imp) {
            if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
            auto* names = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
            auto* slots = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++slots) {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
                auto* ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
                if (strcmp((const char*)ibn->Name, func) == 0) return (void**)&slots->u1.Function;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

}  // namespace

namespace VideoHook {

void Install() {
    g_log = GetPrivateProfileIntA("Coop", "VideoLog", 0, Paths::Ini());
    const int skip = GetPrivateProfileIntA("Coop", "SkipIntroVideos", 0, Paths::Ini());
    if (!g_log && !skip) return;
    void** slot = FindImportSlot("bink2w32.dll", "_BinkOpen@8");
    HMODULE bink = GetModuleHandleW(L"bink2w32.dll");
    void* real = bink ? (void*)GetProcAddress(bink, "_BinkOpen@8") : nullptr;
    if (!slot) { Log("[Video] pid=%lu import slot _BinkOpen@8 not found - no hook\n", GetCurrentProcessId()); return; }
    // The slot must already hold the resolved address: otherwise the loader could still overwrite our stub.
    if (!real || *slot != real) {
        Log("[Video] pid=%lu slot %p holds %p, bink2w32!_BinkOpen@8 is %p - not resolved yet, no hook\n",
            GetCurrentProcessId(), (void*)slot, slot ? *slot : nullptr, real);
        return;
    }
    g_realBinkOpen = (BinkOpen_t)*slot;
    DWORD old = 0;
    VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
    *slot = (void*)&MyBinkOpen;
    VirtualProtect(slot, sizeof(void*), old, &old);
    Log("[Video] pid=%lu t=%llums hooked _BinkOpen@8 slot=%p (rva %08x) real=%p log=%d skip=%d\n", GetCurrentProcessId(),
        ProcessAgeMs(), (void*)slot, (unsigned)((uintptr_t)slot - (uintptr_t)GetModuleHandleW(nullptr)), (void*)g_realBinkOpen,
        g_log, skip);
}

}  // namespace VideoHook
