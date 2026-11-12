// See crash_ctx.h. Everything here runs inside a vectored exception handler on the faulting thread, so it uses no
// CRT stream, no allocation and no lock: Win32 file calls, wsprintfA into a static buffer, VirtualQuery for every
// pointer before it is read.
#include "crash_ctx.h"
#include "paths.h"

#include <windows.h>
#include <cstdint>

namespace {

volatile LONG g_done = 0;
HANDLE        g_file = INVALID_HANDLE_VALUE;
uintptr_t     g_exeLo = 0, g_exeHi = 0;     // AC4BFSP.exe image
uintptr_t     g_asiLo = 0, g_asiHi = 0;     // this plugin's image
char          g_line[512];

void Out(const char* s)
{
    DWORD n = 0;
    if (g_file != INVALID_HANDLE_VALUE) WriteFile(g_file, s, (DWORD)lstrlenA(s), &n, nullptr);
}

bool ImageRange(HMODULE m, uintptr_t* lo, uintptr_t* hi)
{
    if (!m) return false;
    const BYTE* b = (const BYTE*)m;
    const IMAGE_DOS_HEADER* d = (const IMAGE_DOS_HEADER*)b;
    if (d->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(b + d->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    *lo = (uintptr_t)b; *hi = (uintptr_t)b + nt->OptionalHeader.SizeOfImage;
    return true;
}

// Number of readable bytes at p, up to want. 0 = not readable. Never touches the memory itself.
size_t Readable(uintptr_t p, size_t want)
{
    if (p < 0x10000) return 0;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    const uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    const size_t avail = (size_t)(end - p);
    return avail < want ? avail : want;
}

bool InExe(uintptr_t v) { return v >= g_exeLo && v < g_exeHi; }
bool InAsi(uintptr_t v) { return v >= g_asiLo && v < g_asiHi; }

void Hexdump(const char* tag, uintptr_t p, size_t bytes)
{
    const size_t n = Readable(p, bytes) & ~(size_t)3;
    wsprintfA(g_line, "-- %s @%08X (%u readable of %u)\r\n", tag, (unsigned)p, (unsigned)n, (unsigned)bytes);
    Out(g_line);
    for (size_t i = 0; i + 4 <= n; i += 16) {
        char* w = g_line;
        w += wsprintfA(w, "%08X ", (unsigned)(p + i));
        for (size_t k = i; k < i + 16 && k + 4 <= n; k += 4) w += wsprintfA(w, " %08X", *(const uint32_t*)(p + k));
        wsprintfA(w, "\r\n");
        Out(g_line);
    }
}

LONG CALLBACK Veh(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    const CONTEXT* c = ep->ContextRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    // Execute faults, and read/write faults whose instruction is inside the game exe (the plugin's own __try probes
    // fault inside the plugin, and never reach this). At most 3 records per process, one file each: the first
    // fault inside an engine call is usually the interesting one even when a __try upstream swallows it.
    if (r->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    const bool exec = r->ExceptionInformation[0] == 8;
    if (!exec && !InExe((uintptr_t)r->ExceptionAddress)) return EXCEPTION_CONTINUE_SEARCH;
    const LONG nrec = InterlockedIncrement(&g_done);
    if (nrec > 3) return EXCEPTION_CONTINUE_SEARCH;

    char path[MAX_PATH];
    wsprintfA(path, "%s\\AC4Coop_crashctx_%lu_%ld.txt", Paths::LogDir(), GetCurrentProcessId(), nrec);
    g_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) return EXCEPTION_CONTINUE_SEARCH;

    wsprintfA(g_line, "[CrashCtx] %s fault eip=%08X data=%08X tid=%lu tick=%lu exe=%08X..%08X asi=%08X..%08X\r\n",
              exec ? "execute" : (r->ExceptionInformation[0] == 1 ? "write" : "read"), (unsigned)(uintptr_t)r->ExceptionAddress,
              (unsigned)r->ExceptionInformation[1], GetCurrentThreadId(), GetTickCount(),
              (unsigned)g_exeLo, (unsigned)g_exeHi, (unsigned)g_asiLo, (unsigned)g_asiHi);
    Out(g_line);
    wsprintfA(g_line, "eip=%08X esp=%08X ebp=%08X eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X\r\n",
              (unsigned)c->Eip, (unsigned)c->Esp, (unsigned)c->Ebp, (unsigned)c->Eax, (unsigned)c->Ebx,
              (unsigned)c->Ecx, (unsigned)c->Edx, (unsigned)c->Esi, (unsigned)c->Edi);
    Out(g_line);

    // Stack: 0x200 words. Code addresses are tagged; a heap pointer is followed one level so the class of the
    // object it names can be read from its first dword (a vtable inside the exe image).
    Out("-- stack: off value [E exe+rva | A asi+rva | -> first dword of the pointee, * = vtable in the exe]\r\n");
    const uintptr_t sp = c->Esp;
    const size_t stackBytes = Readable(sp, 0x800) & ~(size_t)3;
    static uintptr_t objs[96]; int nobj = 0;
    for (size_t i = 0; i + 4 <= stackBytes; i += 4) {
        const uintptr_t v = *(const uint32_t*)(sp + i);
        if (InExe(v))      wsprintfA(g_line, "+%03X %08X E exe+%X\r\n", (unsigned)i, (unsigned)v, (unsigned)(v - g_exeLo));
        else if (InAsi(v)) wsprintfA(g_line, "+%03X %08X A asi+%X\r\n", (unsigned)i, (unsigned)v, (unsigned)(v - g_asiLo));
        else if (Readable(v, 4) == 4 && (v < sp || v >= sp + 0x10000)) {
            const uintptr_t first = *(const uint32_t*)v;
            wsprintfA(g_line, "+%03X %08X -> %08X%s\r\n", (unsigned)i, (unsigned)v, (unsigned)first, InExe(first) ? " *" : "");
            bool seen = false;
            for (int k = 0; k < nobj; ++k) if (objs[k] == v) { seen = true; break; }
            if (!seen && nobj < 96) objs[nobj++] = v;
        } else continue;
        Out(g_line);
    }

    // The registers' pointees in full (the freed object and the object that points at it are among them).
    const struct { const char* n; uintptr_t v; } regs[] = {
        { "eax", c->Eax }, { "ebx", c->Ebx }, { "ecx", c->Ecx }, { "edx", c->Edx }, { "esi", c->Esi }, { "edi", c->Edi } };
    for (const auto& g : regs)
        if (g.v >= 0x10010 && !InExe(g.v) && !InAsi(g.v)) Hexdump(g.n, g.v - 0x10, 0x310);
    // Every distinct heap object the stack names: 0x240 bytes each (covers the +0x1F4 link of an action state).
    for (int k = 0; k < nobj; ++k) Hexdump("stackobj", objs[k], 0x240);

    Out("[CrashCtx] end\r\n");
    FlushFileBuffers(g_file);
    CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void CrashCtx::Init()
{
    if (!GetPrivateProfileIntA("Coop", "CrashContext", 0, Paths::Ini())) return;
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&CrashCtx::Init, &self);
    if (!ImageRange(GetModuleHandleA(nullptr), &g_exeLo, &g_exeHi)) return;
    ImageRange(self, &g_asiLo, &g_asiHi);
    AddVectoredExceptionHandler(1, Veh);
    OutputDebugStringA("[CrashCtx] recorder armed (execute faults only)\n");
}
