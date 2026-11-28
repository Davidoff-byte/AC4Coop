#include "test.h"

#include <cstring>

namespace t {
int g_pass = 0, g_fail = 0;
namespace {
struct Entry { const char* name; Fn fn; };
Entry g_tests[256];
int   g_n = 0;
}  // namespace
Reg::Reg(const char* name, Fn fn) { if (g_n < 256) g_tests[g_n++] = { name, fn }; }
}  // namespace t

// AC4Coop2Tests.exe [filter]: runs every test whose name contains `filter` (all without one).
int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0;
    for (int i = 0; i < t::g_n; ++i) {
        if (filter && !strstr(t::g_tests[i].name, filter)) continue;
        const int before = t::g_fail;
        t::g_tests[i].fn();
        ++ran;
        std::printf("%-40s %s\n", t::g_tests[i].name, t::g_fail == before ? "ok" : "FAILED");
    }
    std::printf("TESTS: %d passed, %d failed (%d tests)\n", t::g_pass, t::g_fail, ran);
    return t::g_fail;
}
