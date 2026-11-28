#pragma once
// Local monotonic clock (QPC, microseconds). The shared host timeline is net/peerclock (R10).
#include <windows.h>
#include <cstdint>

namespace v2::clock {

inline uint64_t NowUs() {
    static const uint64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (uint64_t)f.QuadPart; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    const uint64_t q = (uint64_t)c.QuadPart;
    return (q / freq) * 1000000ull + (q % freq) * 1000000ull / freq;
}

}  // namespace v2::clock
