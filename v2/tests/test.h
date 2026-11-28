#pragma once
// Minimal offline test runner for v2's pure code (docs/v2/ARCHITECTURE.md s2 tests/). bin\v2\tests\AC4Coop2Tests.exe
// prints every failed CHECK and a last line "TESTS: <checks> passed, <n> failed (<t> tests)"; exit code = failures.
#include <cstdio>

namespace t {
extern int g_pass, g_fail;
using Fn = void (*)();
struct Reg { Reg(const char* name, Fn fn); };
}  // namespace t

#define CHECK(c) do { if (c) ++t::g_pass; else { ++t::g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define TEST(name) static void name(); static t::Reg reg_##name(#name, name); static void name()

// Deterministic PRNG (xorshift32) so every run tests the same sequences.
struct Rng {
    unsigned s;
    explicit Rng(unsigned seed) : s(seed ? seed : 1) {}
    unsigned Next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    unsigned Below(unsigned n) { return n ? Next() % n : 0; }
    bool     Chance(unsigned pct) { return Below(100) < pct; }
};
