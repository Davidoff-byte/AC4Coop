#pragma once
// The only SEH in v2 (ARCHITECTURE.md R2): pure reads of memory we do not own. Never wrap an engine call in these.
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace v2::mem {

// Copies n bytes from addr to out; false (out untouched past the fault) when any byte is unreadable.
inline bool Read(const void* addr, void* out, size_t n) {
    if (!addr) return false;
    __try { memcpy(out, addr, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

template <class T>
inline bool Read(uintptr_t addr, T* out) { return Read((const void*)addr, out, sizeof(T)); }

template <class T>
inline T ReadOr(uintptr_t addr, T fallback) { T v; return Read((const void*)addr, &v, sizeof(T)) ? v : fallback; }

// True when [p, p+n) can be read now (probes the first and last byte and every page between).
inline bool Readable(const void* p, size_t n) {
    if (!p || !n) return false;
    const uintptr_t a = (uintptr_t)p, e = a + n - 1;
    __try {
        volatile uint8_t s = *(const volatile uint8_t*)a;
        for (uintptr_t q = (a & ~(uintptr_t)0xFFF) + 0x1000; q <= e; q += 0x1000) s = *(const volatile uint8_t*)q;
        s = *(const volatile uint8_t*)e;
        (void)s;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The first dword (vtable) of an object, 0 when unreadable.
inline uint32_t Vtable(const void* obj) { return ReadOr<uint32_t>((uintptr_t)obj, 0u); }

// An engine handle {obj, ?, flags}: the engine's own validity rule, obj = h[0] & (h[2] >> 31) (IDA 0x10968C0).
inline void* HandleObject(const void* h) {
    int32_t w[3];
    if (!Read(h, w, sizeof(w))) return nullptr;
    return (void*)(uintptr_t)(w[0] & (w[2] >> 31));
}

}  // namespace v2::mem
