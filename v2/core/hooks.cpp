#include "hooks.h"
#include "log.h"
#include "mem.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace v2::hooks {
namespace {

enum Kind : uint8_t { kJmp, kCall, kBytes, kSlot };
const char* const kKindName[] = { "jmp", "call", "bytes", "slot" };

struct Site {
    const char* owner;
    const char* name;
    uintptr_t   va;
    uint32_t    len;
    Kind        kind;
    uintptr_t   target;
};

constexpr int kMaxSites = 128;
Site   g_sites[kMaxSites];
int    g_n = 0;
size_t g_refused = 0;

bool Overlaps(uintptr_t va, size_t len, const Site** hit) {
    for (int i = 0; i < g_n; ++i) {
        const Site& s = g_sites[i];
        if (va < s.va + s.len && s.va < va + len) { *hit = &s; return true; }
    }
    return false;
}

bool Refuse(const char* owner, const char* name, uintptr_t va, const char* why) {
    ++g_refused;
    LOGE("[Hooks] REFUSED %s/%s at %08X: %s\n", owner, name, (unsigned)va, why);
    return false;
}

bool Admit(const char* owner, const char* name, uintptr_t va, size_t len, const uint8_t* expect) {
    if (g_n >= kMaxSites) return Refuse(owner, name, va, "site table full");
    const Site* hit = nullptr;
    if (Overlaps(va, len, &hit)) {
        char why[160];
        _snprintf_s(why, sizeof(why), _TRUNCATE, "overlaps %s/%s at %08X", hit->owner, hit->name, (unsigned)hit->va);
        return Refuse(owner, name, va, why);
    }
    if (expect) {
        uint8_t cur[32];
        if (len > sizeof(cur) || !mem::Read((const void*)va, cur, len)) return Refuse(owner, name, va, "unreadable");
        if (memcmp(cur, expect, len) != 0) {
            char why[200]; int o = _snprintf_s(why, sizeof(why), _TRUNCATE, "bytes differ:");
            for (size_t i = 0; i < len && o > 0 && o < (int)sizeof(why) - 4; ++i) o += _snprintf_s(why + o, sizeof(why) - o, _TRUNCATE, " %02X", cur[i]);
            return Refuse(owner, name, va, why);
        }
    }
    return true;
}

void Record(const char* owner, const char* name, uintptr_t va, size_t len, Kind kind, uintptr_t target) {
    g_sites[g_n++] = { owner, name, va, (uint32_t)len, kind, target };
    LOGI("[Hooks] %s/%s %s at %08X (%u bytes) -> %08X\n", owner, name, kKindName[kind], (unsigned)va, (unsigned)len,
         (unsigned)target);
}

bool WriteCode(uintptr_t va, const uint8_t* bytes, size_t len) {
    DWORD old = 0;
    if (!VirtualProtect((void*)va, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)va, bytes, len);
    VirtualProtect((void*)va, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)va, len);
    return true;
}

}  // namespace

bool JmpDetour(const char* owner, const char* name, uintptr_t va, const uint8_t* expect, size_t len, void* target,
               uintptr_t* resume) {
    if (len < 5 || len > 32) return Refuse(owner, name, va, "stolen length must be 5..32");
    if (!expect) return Refuse(owner, name, va, "a detour needs its expected bytes");
    if (!Admit(owner, name, va, len, expect)) return false;
    uint8_t code[32];
    memset(code, 0x90, len);
    code[0] = 0xE9;
    const int32_t rel = (int32_t)((uintptr_t)target - (va + 5));
    memcpy(code + 1, &rel, 4);
    if (resume) *resume = va + len;
    if (!WriteCode(va, code, len)) return Refuse(owner, name, va, "VirtualProtect failed");
    Record(owner, name, va, len, kJmp, (uintptr_t)target);
    return true;
}

bool FunctionHook(const char* owner, const char* name, uintptr_t va, const uint8_t* expect, size_t len, void* detour,
                  void** original) {
    if (len < 5 || len > 24) return Refuse(owner, name, va, "stolen length must be 5..24");
    if (!expect || !original) return Refuse(owner, name, va, "a function hook needs its expected bytes");
    if (!Admit(owner, name, va, len, expect)) return false;
    // Trampolines live in one executable page, 32 bytes each: stolen bytes, then jmp [slot] with the resume address.
    static uint8_t* s_page = nullptr; static size_t s_used = 0;
    if (!s_page) s_page = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!s_page || s_used + 40 > 4096) return Refuse(owner, name, va, "trampoline page exhausted");
    uint8_t* t = s_page + s_used; s_used += 40;
    memcpy(t, expect, len);
    t[len] = 0xFF; t[len + 1] = 0x25;                            // jmp dword ptr [slot]
    const uintptr_t slot = (uintptr_t)(t + 32);
    memcpy(t + len + 2, &slot, 4);
    const uintptr_t resume = va + len;
    memcpy(t + 32, &resume, 4);
    FlushInstructionCache(GetCurrentProcess(), t, 40);
    *original = t;                                               // set before the site is patched
    uint8_t code[24];
    memset(code, 0x90, len);
    code[0] = 0xE9;
    const int32_t rel = (int32_t)((uintptr_t)detour - (va + 5));
    memcpy(code + 1, &rel, 4);
    if (!WriteCode(va, code, len)) { *original = nullptr; return Refuse(owner, name, va, "VirtualProtect failed"); }
    Record(owner, name, va, len, kJmp, (uintptr_t)detour);
    return true;
}

bool CallRedirect(const char* owner, const char* name, uintptr_t va, void* target, uintptr_t* original) {
    uint8_t cur[5];
    if (!mem::Read((const void*)va, cur, 5) || cur[0] != 0xE8) return Refuse(owner, name, va, "not a call rel32");
    if (!Admit(owner, name, va, 5, cur)) return false;
    int32_t oldRel = 0; memcpy(&oldRel, cur + 1, 4);
    if (original) *original = va + 5 + oldRel;
    uint8_t code[5] = { 0xE8 };
    const int32_t rel = (int32_t)((uintptr_t)target - (va + 5));
    memcpy(code + 1, &rel, 4);
    if (!WriteCode(va, code, 5)) return Refuse(owner, name, va, "VirtualProtect failed");
    Record(owner, name, va, 5, kCall, (uintptr_t)target);
    return true;
}

bool BytePatch(const char* owner, const char* name, uintptr_t va, const uint8_t* expect, const uint8_t* bytes, size_t len) {
    if (!expect) return Refuse(owner, name, va, "a patch needs its expected bytes");
    if (!Admit(owner, name, va, len, expect)) return false;
    if (!WriteCode(va, bytes, len)) return Refuse(owner, name, va, "VirtualProtect failed");
    Record(owner, name, va, len, kBytes, 0);
    return true;
}

bool PointerSlot(const char* owner, const char* name, void** slot, void* replacement, void** original, uintptr_t expect) {
    const uintptr_t va = (uintptr_t)slot;
    void* cur = nullptr;
    if (!mem::Read(slot, &cur, sizeof(cur))) return Refuse(owner, name, va, "slot unreadable");
    if (expect && (uintptr_t)cur != expect) return Refuse(owner, name, va, "slot holds an unexpected pointer");
    if (!Admit(owner, name, va, sizeof(void*), nullptr)) return false;
    if (original) *original = cur;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return Refuse(owner, name, va, "VirtualProtect failed");
    *slot = replacement;
    VirtualProtect(slot, sizeof(void*), old, &old);
    Record(owner, name, va, sizeof(void*), kSlot, (uintptr_t)replacement);
    return true;
}

void LogManifest() {
    LOGI("[Hooks] manifest: %d site(s), %u refused\n", g_n, (unsigned)g_refused);
    for (int i = 0; i < g_n; ++i) {
        const Site& s = g_sites[i];
        LOGI("[Hooks]   %-10s %-24s %-5s %08X len=%u\n", s.owner, s.name, kKindName[s.kind], (unsigned)s.va, s.len);
    }
}

size_t Count() { return (size_t)g_n; }
size_t Refused() { return g_refused; }

}  // namespace v2::hooks
