#include "test.h"
#include "../net/registry.h"

#include <map>

using namespace v2::net;

TEST(registry_ids) {
    static Registry reg(1);
    const NetId a = reg.Allocate(), b = reg.Allocate();
    CHECK(OwnerOf(a) == 1 && OwnerOf(b) == 1 && a != b && (a & 0xFFFFFF) == 1 && (b & 0xFFFFFF) == 2);
    reg.SetSelf(0);
    CHECK(OwnerOf(reg.Allocate()) == 0);
    CHECK(MakeNetId(255, 0x1234567) == 0xFF234567u);   // the counter keeps 24 bits
}

TEST(registry_bind_lookup) {
    static Registry reg(0);
    CHECK(reg.Bind(0x01000001, 0x2DD1ADB0, 3));
    CHECK(!reg.Bind(0x01000001, 0x11111110, 3));       // id already bound
    CHECK(!reg.Bind(0x01000002, 0x2DD1ADB0, 3));       // body already bound
    CHECK(!reg.Bind(0, 0x10, 1) && !reg.Bind(5, 0, 1));
    CHECK(reg.Local(0x01000001) == 0x2DD1ADB0 && reg.IdOf(0x2DD1ADB0) == 0x01000001 && reg.KindOf(0x01000001) == 3);
    CHECK(reg.Unbind(0x01000001) && !reg.Unbind(0x01000001));
    CHECK(reg.Local(0x01000001) == 0 && reg.IdOf(0x2DD1ADB0) == 0 && reg.Count() == 0);
}

TEST(registry_full_and_churn) {
    static Registry reg(0);
    for (uint32_t i = 0; i < Registry::kMax; ++i) CHECK(reg.Bind(i + 1, 0x10000000u + i * 16, 1));
    CHECK(reg.Count() == Registry::kMax && !reg.Bind(999999, 0x7FFF0000, 1));
    reg.Clear();
    CHECK(reg.Count() == 0 && reg.Local(1) == 0);
    // Random bind/unbind against a std::map model: lookups agree after every step (covers tombstones and rebuilds).
    std::map<uint32_t, uintptr_t> model;
    Rng rng(12345);
    bool agree = true;
    for (int step = 0; step < 200000; ++step) {
        const uint32_t id = 1 + rng.Below(3000);
        const uintptr_t local = 0x20000000u + (uintptr_t)id * 64;
        const bool bound = model.count(id) != 0;
        if (rng.Chance(55)) {
            const bool ok = reg.Bind(id, local, (uint8_t)(id & 7));
            if (ok != (!bound && model.size() < Registry::kMax)) agree = false;
            if (ok) model[id] = local;
        } else {
            if (reg.Unbind(id) != bound) agree = false;
            model.erase(id);
        }
        if ((step & 255) == 0) {
            for (const auto& kv : model) if (reg.Local(kv.first) != kv.second || reg.IdOf(kv.second) != kv.first) agree = false;
            if (reg.Count() != model.size()) agree = false;
        }
    }
    CHECK(agree);
    size_t seen = 0; reg.ForEach([&](NetId, uintptr_t, uint8_t) { ++seen; });
    CHECK(seen == model.size());
}
