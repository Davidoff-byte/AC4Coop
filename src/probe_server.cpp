// Probe server: x86 port of the AU-RW ACU tap (tools/native_tap/tap.cpp). See probe_server.h for the commands.
//
// Differences from the x64 original, all forced by x86 calling conventions:
//  - The x64 thunk calls the original and replays a fixed block of stack arguments; that only works because the x64
//    caller always cleans the stack. On x86 the callee pops its own arguments for stdcall/thiscall, so here the entry
//    thunk records and then RETURNS INTO the MinHook trampoline (tail jump, stack untouched), and exit capture swaps
//    the return address for an exit thunk through a per-thread shadow stack. Both work for any convention.
//  - Debug-register probes are not ported (phase1 owns DR traps for its own diagnostics).
// No engine function is called by this module.
#include "probe_server.h"
#include "paths.h"
#include "../third_party/minhook/include/MinHook.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

void Phase1_LogLine(const char* s);

namespace {

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;

void Log(const char* fmt, ...)
{
    char b[300];
    va_list a; va_start(a, fmt); _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a); va_end(a);
    Phase1_LogLine(b);
}

bool SafeCopy(void* dst, const void* src, size_t n)
{
    __try { memcpy(dst, src, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool Plausible(u32 p) { return p >= 0x10000 && p < 0x7FFF0000; }

// ---- hooks ---------------------------------------------------------------------------------------------------
enum { kHooks = 64, kArgs = 8, kSnapMax = 256, kSnapsPerEv = 4, kRing = 4096, kSpecs = 256, kShadow = 256 };

struct HookInfo { volatile LONG active; volatile LONG log; u32 va; u32 tramp; volatile LONG entries, exits; bool exitWanted; char label[48]; };
HookInfo g_hooks[kHooks];
u8* g_stubs = nullptr;            // kHooks x 16 bytes: push imm32 id ; jmp rel32 EntryThunk

// Saved by: [stub] push id ; [thunk] pushfd ; pushad  (pushad stores edi lowest).
struct Regs { u32 edi, esi, ebp, esp_, ebx, edx, ecx, eax, eflags, slot, ret, args[kArgs]; };
// Exit thunk: push 0 ; pushfd ; pushad.
struct ExitRegs { u32 edi, esi, ebp, esp_, ebx, edx, ecx, eax, eflags, slot; };

struct Op { char kind; int32_t k; };
struct Spec { volatile LONG live; u32 hook; u8 phase; u16 snapId; u32 len; int start; int nops; Op ops[16]; };
Spec g_specs[kSpecs];
volatile LONG g_specCount = 0;

struct Snap { u16 id; u8 ok; u8 pad; u16 len; u16 pad2; u8 data[kSnapMax]; };
struct Ev { volatile u32 seq; u32 tid; u64 qpc; u16 hook; u8 phase; u8 nsnap; u32 ecx, edx, eax, eflags, esp, ret; u32 args[kArgs]; Snap snaps[kSnapsPerEv]; };
Ev* g_ring = nullptr;
volatile LONG g_evCount = 0, g_dropped = 0;

struct Shadow { u32 ret, hook, esp, ecx, args[kArgs]; };
struct ShadowStack { u32 n; Shadow e[kShadow]; };
__declspec(thread) ShadowStack t_shadow;

extern "C" void __cdecl ProbeOnEnter(Regs* r);
extern "C" u32 __cdecl ProbeOnExit(ExitRegs* r);

__declspec(naked) void EntryThunk()
{
    __asm {
        pushfd
        pushad
        push esp
        call ProbeOnEnter      // rewrites Regs.slot = trampoline, Regs.ret = ExitThunk when exit capture is on
        add  esp, 4
        popad
        popfd
        ret                    // pops the slot: continues in the trampoline with the caller's frame intact
    }
}

__declspec(naked) void ExitThunk()
{
    __asm {
        push 0                 // becomes the real return address
        pushfd
        pushad
        push esp
        call ProbeOnExit
        add  esp, 4
        popad
        popfd
        ret
    }
}

bool EvalStart(const Spec& s, u32 ecx, u32 edx, u32 eax, u32 esp, const u32* args, u32* out)
{
    u32 v;
    switch (s.start) {
    case 0: v = ecx; break; case 1: v = edx; break; case 2: v = eax; break; case 3: v = esp; break;
    default: if (s.start < 4 || s.start >= 4 + kArgs) return false; v = args[s.start - 4];
    }
    for (int i = 0; i < s.nops; ++i) {
        if (s.ops[i].kind == '+') v += (u32)s.ops[i].k;
        else { if (!Plausible(v)) return false; u32 t; if (!SafeCopy(&t, (void*)(uintptr_t)v, 4)) return false; v = t; }
    }
    *out = v; return true;
}

void Record(u32 id, u8 phase, u32 ecx, u32 edx, u32 eax, u32 eflags, u32 esp, u32 ret, const u32* args)
{
    if (!g_ring || !g_hooks[id].log) return;
    LONG idx = InterlockedIncrement(&g_evCount) - 1;
    Ev& e = g_ring[(u32)idx % kRing];
    e.seq = 0;
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    e.tid = GetCurrentThreadId(); e.qpc = (u64)q.QuadPart; e.hook = (u16)id; e.phase = phase;
    e.ecx = ecx; e.edx = edx; e.eax = eax; e.eflags = eflags; e.esp = esp; e.ret = ret;
    memcpy(e.args, args, sizeof(e.args));
    u8 ns = 0;
    LONG n = g_specCount;
    for (LONG i = 0; i < n && ns < kSnapsPerEv; ++i) {
        const Spec& s = g_specs[i];
        if (!s.live || s.hook != id || s.phase != phase) continue;
        Snap& sn = e.snaps[ns++];
        sn.id = s.snapId; sn.len = (u16)s.len;
        u32 a = 0;
        sn.ok = (EvalStart(s, ecx, edx, eax, esp, args, &a) && Plausible(a) && SafeCopy(sn.data, (void*)(uintptr_t)a, s.len)) ? 1 : 0;
        if (!sn.ok) memset(sn.data, 0, s.len);
    }
    e.nsnap = ns;
    e.seq = (u32)idx + 1;
}

}  // namespace

extern "C" void __cdecl ProbeOnEnter(Regs* r)
{
    u32 id = r->slot;
    HookInfo& h = g_hooks[id];
    InterlockedIncrement(&h.entries);
    u32 entryEsp = (u32)(uintptr_t)&r->ret;
    Record(id, 0, r->ecx, r->edx, r->eax, r->eflags, entryEsp, r->ret, r->args);
    if (h.exitWanted && h.active) {
        ShadowStack& ss = t_shadow;
        if (ss.n < kShadow) {
            Shadow& s = ss.e[ss.n++];
            s.ret = r->ret; s.hook = id; s.esp = entryEsp; s.ecx = r->ecx;
            memcpy(s.args, r->args, sizeof(s.args));
            r->ret = (u32)(uintptr_t)&ExitThunk;
        }
    }
    r->slot = h.tramp;
}

extern "C" u32 __cdecl ProbeOnExit(ExitRegs* r)
{
    // esp after the callee's `ret N` = entryEsp + 4 + N. Pick the newest shadow entry consistent with that; entries
    // above it belong to frames that were unwound by an exception and never returned.
    ShadowStack& ss = t_shadow;
    u32 now = (u32)(uintptr_t)(&r->slot + 1);
    while (ss.n > 0) {
        Shadow& s = ss.e[ss.n - 1];
        if (now > s.esp && now <= s.esp + 4 + 0x400) break;
        --ss.n;
    }
    if (ss.n == 0) {   // cannot happen unless the shadow stack was corrupted; nowhere safe to return to
        Log("[Probe] FATAL: exit thunk without a shadow entry (tid %lu)\n", GetCurrentThreadId());
        TerminateProcess(GetCurrentProcess(), 0xDEAD);
    }
    Shadow s = ss.e[--ss.n];
    InterlockedIncrement(&g_hooks[s.hook].exits);
    Record(s.hook, 1, s.ecx, r->edx, r->eax, r->eflags, now, s.ret, s.args);
    r->slot = s.ret;
    return s.ret;
}

namespace {

// ---- write tracing by page protection (AU-RW WWATCH) -------------------------------------------------------------
struct WWatch { u32 addr, len; };
struct WHit { u32 seq, eip, addr, esp, tid, len; u32 stack[8]; };
WWatch g_ww[8]; int g_wwN = 0;
u32 g_wpages[8]; DWORD g_wprot[8]; int g_wpageN = 0;
u32 g_everPages[64]; volatile LONG g_everN = 0;
enum { kWHits = 65536 };
WHit* g_whits = nullptr; volatile LONG g_whitN = 0;
volatile LONG g_wactive = 0, g_wstopping = 0;
__declspec(thread) u32 t_rearm = 0;

LONG CALLBACK WriteVeh(EXCEPTION_POINTERS* x)
{
    EXCEPTION_RECORD* er = x->ExceptionRecord;
    if (er->ExceptionCode == EXCEPTION_SINGLE_STEP && t_rearm) {
        if (!g_wstopping && g_wactive) { DWORD old; VirtualProtect((void*)(uintptr_t)t_rearm, 0x1000, PAGE_READONLY, &old); }
        t_rearm = 0; return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2 && er->ExceptionInformation[0] == 1) {
        u32 a = (u32)er->ExceptionInformation[1], page = a & ~0xFFFu;
        bool mine = false; LONG en = g_everN;
        for (LONG i = 0; i < en; ++i) if (g_everPages[i] == page) mine = true;
        if (!mine) return EXCEPTION_CONTINUE_SEARCH;
        bool recording = g_wactive && !g_wstopping;
        if (recording) {
            for (int i = 0; i < g_wwN; ++i) {
                if (a + 16 > g_ww[i].addr && a < g_ww[i].addr + g_ww[i].len) {
                    LONG k = InterlockedIncrement(&g_whitN) - 1;
                    if (g_whits && k < kWHits) {
                        WHit& h = g_whits[k];
                        h.seq = (u32)k; h.eip = x->ContextRecord->Eip; h.addr = a; h.esp = x->ContextRecord->Esp;
                        h.tid = GetCurrentThreadId(); h.len = g_ww[i].len;
                        if (!SafeCopy(h.stack, (void*)(uintptr_t)h.esp, sizeof(h.stack))) memset(h.stack, 0, sizeof(h.stack));
                    }
                    break;
                }
            }
        }
        DWORD old; VirtualProtect((void*)(uintptr_t)page, 0x1000, PAGE_READWRITE, &old);
        if (!recording) return EXCEPTION_CONTINUE_EXECUTION;
        t_rearm = page; x->ContextRecord->EFlags |= 0x100; return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void StopWrites()
{
    if (!g_wactive) return;
    InterlockedExchange(&g_wstopping, 1);
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < g_wpageN; ++i) { DWORD old; VirtualProtect((void*)(uintptr_t)g_wpages[i], 0x1000, g_wprot[i], &old); }
        Sleep(40);
    }
    g_wactive = 0; g_wpageN = g_wwN = 0; Sleep(20); InterlockedExchange(&g_wstopping, 0);
}

// ---- commands --------------------------------------------------------------------------------------------------
std::string Hex(u32 v) { char b[16]; sprintf_s(b, "\"0x%x\"", v); return b; }
bool ParseHex(const std::string& s, u32* v) { char* e; *v = strtoul(s.c_str(), &e, 16); return !s.empty() && *e == 0; }
u8 HexV(char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }
std::string HexBytes(const u8* p, size_t n) { static const char* d = "0123456789abcdef"; std::string o; o.reserve(n * 2); for (size_t i = 0; i < n; ++i) { o += d[p[i] >> 4]; o += d[p[i] & 15]; } return o; }
std::string Js(const char* s) { std::string o = "\""; for (; *s; ++s) { if (*s == '"' || *s == '\\') o += '\\'; if ((u8)*s >= 32) o += *s; } return o + "\""; }
std::vector<std::string> Split(const std::string& s) { std::istringstream i(s); std::vector<std::string> o; std::string t; while (i >> t) o.push_back(t); return o; }
const char* kStarts[] = { "ecx", "edx", "eax", "esp", "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7" };

std::string EvJson(const Ev& e, u32 seq)
{
    std::ostringstream o;
    o << "{\"seq\":" << seq << ",\"tid\":" << e.tid << ",\"qpc\":" << e.qpc << ",\"hook\":" << e.hook << ",\"phase\":\"" << (e.phase ? "exit" : "entry")
      << "\",\"ecx\":" << Hex(e.ecx) << ",\"edx\":" << Hex(e.edx) << ",\"eax\":" << Hex(e.eax) << ",\"eflags\":" << Hex(e.eflags)
      << ",\"esp\":" << Hex(e.esp) << ",\"ret\":" << Hex(e.ret) << ",\"args\":[";
    for (int i = 0; i < kArgs; ++i) o << (i ? "," : "") << Hex(e.args[i]);
    o << "],\"snaps\":[";
    for (int i = 0; i < e.nsnap && i < kSnapsPerEv; ++i)
        o << (i ? "," : "") << "{\"id\":" << e.snaps[i].id << ",\"ok\":" << (int)e.snaps[i].ok << ",\"hex\":\"" << HexBytes(e.snaps[i].data, e.snaps[i].len) << "\"}";
    o << "]}";
    return o.str();
}

std::string Handle(const std::string& line)
{
    std::vector<std::string> t = Split(line);
    if (t.empty()) return "{\"ok\":false,\"error\":\"empty\"}";
    const std::string& c = t[0];
    std::ostringstream o;
    if (c == "STATUS") {
        o << "{\"ok\":true,\"pid\":" << GetCurrentProcessId() << ",\"events\":" << g_evCount << ",\"ring\":" << kRing << ",\"dropped\":" << g_dropped
          << ",\"specs\":" << g_specCount << ",\"write_hits\":" << g_whitN << ",\"hooks\":[";
        bool first = true;
        for (int i = 0; i < kHooks; ++i) if (g_hooks[i].active) {
            o << (first ? "" : ",") << "{\"id\":" << i << ",\"va\":" << Hex(g_hooks[i].va) << ",\"label\":" << Js(g_hooks[i].label)
              << ",\"entries\":" << g_hooks[i].entries << ",\"exits\":" << g_hooks[i].exits << ",\"exit\":" << (g_hooks[i].exitWanted ? "true" : "false") << "}";
            first = false;
        }
        o << "]}"; return o.str();
    }
    if (c == "READ" && t.size() == 3) {
        u32 a; size_t n = strtoul(t[2].c_str(), nullptr, 10);
        if (!ParseHex(t[1], &a) || n < 1 || n > 65536) return "{\"ok\":false,\"error\":\"args\"}";
        std::vector<u8> v(n);
        if (!Plausible(a) || !SafeCopy(v.data(), (void*)(uintptr_t)a, n)) return "{\"ok\":false,\"error\":\"unreadable\"}";
        return "{\"ok\":true,\"hex\":\"" + HexBytes(v.data(), n) + "\"}";
    }
    if (c == "POKE" && t.size() == 3) {
        u32 a; if (!ParseHex(t[1], &a) || !Plausible(a) || t[2].size() % 2 || t[2].size() > 8192) return "{\"ok\":false,\"error\":\"args\"}";
        std::vector<u8> v(t[2].size() / 2);
        for (size_t i = 0; i < v.size(); ++i) v[i] = (u8)(HexV(t[2][2 * i]) << 4 | HexV(t[2][2 * i + 1]));
        return SafeCopy((void*)(uintptr_t)a, v.data(), v.size()) ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"unwritable\"}";
    }
    if (c == "HOOK" && t.size() >= 5) {
        int id = atoi(t[1].c_str()); u32 va;
        if (id < 0 || id >= kHooks || !ParseHex(t[2], &va)) return "{\"ok\":false,\"error\":\"args\"}";
        if (g_hooks[id].active) return "{\"ok\":false,\"error\":\"id in use\"}";
        const std::string& ex = t[3];
        if (ex.empty() || ex.size() % 2 || ex.size() > 64) return "{\"ok\":false,\"error\":\"expected bytes\"}";
        u8 cur[32]; size_t n = ex.size() / 2;
        if (!SafeCopy(cur, (void*)(uintptr_t)va, n)) return "{\"ok\":false,\"error\":\"unreadable target\"}";
        for (size_t i = 0; i < n; ++i)
            if (cur[i] != (u8)(HexV(ex[2 * i]) << 4 | HexV(ex[2 * i + 1])))
                return "{\"ok\":false,\"error\":\"byte guard mismatch\",\"live\":\"" + HexBytes(cur, n) + "\"}";
        u8* s = g_stubs + 16 * id;
        s[0] = 0x68; *(u32*)(s + 1) = (u32)id;                                             // push id
        s[5] = 0xE9; *(int32_t*)(s + 6) = (int32_t)((u8*)&EntryThunk - (s + 10));          // jmp EntryThunk
        FlushInstructionCache(GetCurrentProcess(), s, 16);
        HookInfo& h = g_hooks[id];
        h.va = va; h.entries = h.exits = 0; h.log = 1; h.exitWanted = (t.size() > 5 && t[5] == "exit");
        strncpy_s(h.label, t[4].c_str(), _TRUNCATE);
        void* tramp = nullptr;
        MH_STATUS st = MH_CreateHook((void*)(uintptr_t)va, s, &tramp);
        if (st != MH_OK) { o << "{\"ok\":false,\"error\":\"MH_CreateHook " << MH_StatusToString(st) << "\"}"; return o.str(); }
        h.tramp = (u32)(uintptr_t)tramp;
        InterlockedExchange(&h.active, 1);
        st = MH_EnableHook((void*)(uintptr_t)va);
        if (st != MH_OK) { InterlockedExchange(&h.active, 0); MH_RemoveHook((void*)(uintptr_t)va); o << "{\"ok\":false,\"error\":\"MH_EnableHook " << MH_StatusToString(st) << "\"}"; return o.str(); }
        Log("[Probe] HOOK %d at 0x%08X %s%s\n", id, va, h.label, h.exitWanted ? " (exit)" : "");
        o << "{\"ok\":true,\"id\":" << id << ",\"va\":" << Hex(va) << ",\"trampoline\":" << Hex(h.tramp) << "}"; return o.str();
    }
    if (c == "UNHOOK" && t.size() == 2) {
        int id = atoi(t[1].c_str());
        if (id < 0 || id >= kHooks || !g_hooks[id].active) return "{\"ok\":false,\"error\":\"not active\"}";
        // Disable only: the trampoline and the stub stay valid for threads still inside, and pending exits keep working.
        MH_DisableHook((void*)(uintptr_t)g_hooks[id].va);
        g_hooks[id].exitWanted = false;
        InterlockedExchange(&g_hooks[id].active, 0);
        Log("[Probe] UNHOOK %d\n", id);
        return "{\"ok\":true}";
    }
    if (c == "SNAP" && t.size() >= 6) {
        LONG n = g_specCount; if (n >= kSpecs) return "{\"ok\":false,\"error\":\"spec table full\"}";
        Spec& s = g_specs[n];
        s.hook = (u32)atoi(t[1].c_str()); s.phase = (t[2] == "exit") ? 1 : 0; s.snapId = (u16)atoi(t[3].c_str());
        s.len = (u32)strtoul(t[4].c_str(), nullptr, 10); s.start = -1;
        for (int i = 0; i < 12; ++i) if (t[5] == kStarts[i]) s.start = i;
        if (s.hook >= kHooks || s.len < 1 || s.len > kSnapMax || s.start < 0) return "{\"ok\":false,\"error\":\"args\"}";
        s.nops = 0;
        for (size_t i = 6; i < t.size() && s.nops < 16; ++i) {
            if (t[i] == "*") { s.ops[s.nops++] = { '*', 0 }; continue; }
            u32 k; bool neg = t[i][0] == '-';
            if ((t[i][0] != '+' && !neg) || !ParseHex(t[i].substr(1), &k)) return "{\"ok\":false,\"error\":\"op\"}";
            s.ops[s.nops++] = { '+', neg ? -(int32_t)k : (int32_t)k };
        }
        InterlockedExchange(&s.live, 1);
        InterlockedExchange(&g_specCount, n + 1);
        o << "{\"ok\":true,\"spec\":" << n << "}"; return o.str();
    }
    if (c == "CLEARSNAPS") { for (LONG i = 0; i < g_specCount; ++i) g_specs[i].live = 0; return "{\"ok\":true}"; }
    if (c == "LOG" && t.size() == 3) { int id = atoi(t[1].c_str()); if (id < 0 || id >= kHooks) return "{\"ok\":false}"; g_hooks[id].log = atoi(t[2].c_str()); return "{\"ok\":true}"; }
    if (c == "RESET") { InterlockedExchange(&g_evCount, 0); InterlockedExchange(&g_whitN, 0); if (g_ring) memset(g_ring, 0, sizeof(Ev) * kRing); return "{\"ok\":true}"; }
    if (c == "EVENTS") {
        int want = t.size() > 1 ? atoi(t[1].c_str()) : 64; if (want < 1) want = 1; if (want > 256) want = 256;
        LONG total = g_evCount; LONG from = total - want; if (from < 0) from = 0; if (total - from > kRing) from = total - kRing;
        o << "{\"ok\":true,\"total\":" << total << ",\"events\":[";
        bool first = true;
        static Ev copy;
        for (LONG i = from; i < total; ++i) {
            const Ev& e = g_ring[(u32)i % kRing];
            if (e.seq != (u32)i + 1) continue;
            copy = e;
            if (copy.seq != (u32)i + 1) continue;
            o << (first ? "" : ",") << EvJson(copy, (u32)i); first = false;
        }
        o << "]}"; return o.str();
    }
    if (c == "WWATCH" && t.size() == 3) {
        u32 a; u32 l = strtoul(t[2].c_str(), nullptr, 10);
        if (!ParseHex(t[1], &a) || !Plausible(a) || l < 1 || l > 4096 || g_wwN >= 8 || g_wpageN >= 8) return "{\"ok\":false,\"error\":\"args\"}";
        if (!g_whits) g_whits = (WHit*)VirtualAlloc(nullptr, sizeof(WHit) * kWHits, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        u32 page = a & ~0xFFFu; bool known = false;
        for (int i = 0; i < g_wpageN; ++i) known |= g_wpages[i] == page;
        g_ww[g_wwN++] = { a, l };
        if (!known) {
            MEMORY_BASIC_INFORMATION mbi; if (!VirtualQuery((void*)(uintptr_t)page, &mbi, sizeof(mbi))) return "{\"ok\":false,\"error\":\"query\"}";
            bool seen = false; LONG en = g_everN; for (LONG i = 0; i < en; ++i) seen |= g_everPages[i] == page;
            if (!seen && en < 64) { g_everPages[en] = page; InterlockedExchange(&g_everN, en + 1); }
            g_wprot[g_wpageN] = mbi.Protect; g_wpages[g_wpageN++] = page; InterlockedExchange(&g_wactive, 1);
            DWORD old; if (!VirtualProtect((void*)(uintptr_t)page, 0x1000, PAGE_READONLY, &old)) { --g_wpageN; return "{\"ok\":false,\"error\":\"protect\"}"; }
        }
        return "{\"ok\":true}";
    }
    if (c == "WSTOP") { StopWrites(); return "{\"ok\":true}"; }
    if (c == "WHITS") {
        int want = t.size() > 1 ? atoi(t[1].c_str()) : 64; if (want < 1) want = 1; if (want > 512) want = 512;
        LONG total = g_whitN; if (total > kWHits) total = kWHits; LONG from = total - want; if (from < 0) from = 0;
        o << "{\"ok\":true,\"total\":" << g_whitN << ",\"hits\":[";
        for (LONG i = from; i < total && g_whits; ++i) {
            const WHit& h = g_whits[i];
            o << (i > from ? "," : "") << "{\"seq\":" << h.seq << ",\"eip\":" << Hex(h.eip) << ",\"addr\":" << Hex(h.addr) << ",\"tid\":" << h.tid << ",\"stack\":[";
            for (int k = 0; k < 8; ++k) o << (k ? "," : "") << Hex(h.stack[k]);
            o << "]}";
        }
        o << "]}"; return o.str();
    }
    if (c == "UNLOAD") {
        StopWrites();
        for (int i = 0; i < kHooks; ++i) if (g_hooks[i].active) { MH_DisableHook((void*)(uintptr_t)g_hooks[i].va); g_hooks[i].exitWanted = false; g_hooks[i].active = 0; }
        return "{\"ok\":true,\"quiesced\":true}";
    }
    return "{\"ok\":false,\"error\":\"unknown command\"}";
}

DWORD WINAPI PipeThread(void*)
{
    char name[64];
    sprintf_s(name, "\\\\.\\pipe\\ac4probe-%lu", GetCurrentProcessId());
    static char in[65536];
    for (;;) {
        HANDLE p = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                    1, 1 << 20, 1 << 16, 1000, nullptr);
        if (p == INVALID_HANDLE_VALUE) { Sleep(500); continue; }
        if (ConnectNamedPipe(p, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            DWORD n = 0;
            if (ReadFile(p, in, sizeof(in) - 1, &n, nullptr) && n) {
                in[n] = 0;
                std::string line(in);
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
                std::string out = Handle(line) + "\n";
                DWORD w = 0; WriteFile(p, out.data(), (DWORD)out.size(), &w, nullptr);
                char ack[8]; DWORD k = 0; ReadFile(p, ack, sizeof(ack), &k, nullptr);
            }
        }
        FlushFileBuffers(p); DisconnectNamedPipe(p); CloseHandle(p);
    }
}

}  // namespace

void ProbeServer::Init()
{
    if (!GetPrivateProfileIntA("Probe", "Enabled", 0, Paths::Ini())) return;
    g_stubs = (u8*)VirtualAlloc(nullptr, 16 * kHooks, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    g_ring = (Ev*)VirtualAlloc(nullptr, sizeof(Ev) * kRing, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    MH_STATUS st = MH_Initialize();
    if (!g_stubs || !g_ring || (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)) { Log("[Probe] init failed (MinHook %d)\n", (int)st); return; }
    AddVectoredExceptionHandler(1, WriteVeh);
    HANDLE th = CreateThread(nullptr, 0, PipeThread, nullptr, 0, nullptr);
    if (th) CloseHandle(th);
    Log("[Probe] on: pipe \\\\.\\pipe\\ac4probe-%lu, ring %u events x %u bytes\n", GetCurrentProcessId(), (unsigned)kRing, (unsigned)sizeof(Ev));
}
