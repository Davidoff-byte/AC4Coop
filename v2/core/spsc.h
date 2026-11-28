#pragma once
// Bounded single-producer / single-consumer ring of fixed-size items: how the game tick and the net thread hand data to
// each other without locks (docs/v2/ARCHITECTURE.md s3). One thread only pushes, one thread only pops. N must be a
// power of two. A full ring refuses the push (the producer counts the drop); nothing blocks.
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace v2 {

template <class T, size_t N>
class Spsc {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "N must be a power of two");
public:
    // Producer: a slot to fill in place, then Commit(). nullptr = full.
    T* Reserve() {
        const uint32_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) >= N) return nullptr;
        return &items_[h & (N - 1)];
    }
    void Commit() { head_.store(head_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }
    bool Push(const T& v) { T* s = Reserve(); if (!s) return false; *s = v; Commit(); return true; }

    // Consumer: the oldest item (nullptr = empty); Pop() releases it after use.
    T* Front() {
        const uint32_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return nullptr;
        return &items_[t & (N - 1)];
    }
    void Pop() { tail_.store(tail_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

    size_t Size() const { return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire); }

private:
    alignas(64) std::atomic<uint32_t> head_{0};   // written by the producer
    alignas(64) std::atomic<uint32_t> tail_{0};   // written by the consumer
    T items_[N];
};

}  // namespace v2
