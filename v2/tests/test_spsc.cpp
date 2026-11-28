#include "test.h"
#include "../core/spsc.h"

#include <thread>

TEST(spsc_basic) {
    static v2::Spsc<int, 4> q;
    CHECK(q.Front() == nullptr && q.Size() == 0);
    CHECK(q.Push(1) && q.Push(2) && q.Push(3) && q.Push(4));
    CHECK(!q.Push(5) && q.Size() == 4);                 // full: refused
    CHECK(*q.Front() == 1); q.Pop();
    CHECK(q.Push(5));
    int sum = 0, n = 0;
    while (int* v = q.Front()) { sum += *v; ++n; q.Pop(); }
    CHECK(n == 4 && sum == 2 + 3 + 4 + 5);
}

TEST(spsc_two_threads) {
    // 2M items through a 1024 ring between two threads: every item once, in order.
    struct Item { uint32_t seq; uint32_t check; };
    static v2::Spsc<Item, 1024> q;
    static constexpr uint32_t kN = 2000000;
    std::thread producer([] {
        for (uint32_t i = 0; i < kN;) {
            if (Item* s = q.Reserve()) { s->seq = i; s->check = i * 2654435761u; q.Commit(); ++i; }
            else std::this_thread::yield();
        }
    });
    uint32_t expect = 0; bool ok = true;
    while (expect < kN) {
        if (Item* v = q.Front()) {
            if (v->seq != expect || v->check != expect * 2654435761u) ok = false;
            q.Pop(); ++expect;
        } else std::this_thread::yield();
    }
    producer.join();
    CHECK(ok && expect == kN && q.Size() == 0);
}
