#pragma once
// One identity for every replicated object (docs/v2/ARCHITECTURE.md R9, s5): NetId = owner peer id << 24 | the owner's
// counter. The owner allocates; every machine binds the id to its local body (an opaque uintptr_t: an entity, a Human,
// a ship). Used by the game tick only (no locks). Pure code, unit-tested in v2/tests.
#include <cstddef>
#include <cstdint>

namespace v2::net {

using NetId = uint32_t;   // 0 = none
constexpr NetId   MakeNetId(uint8_t owner, uint32_t counter) { return ((uint32_t)owner << 24) | (counter & 0xFFFFFFu); }
constexpr uint8_t OwnerOf(NetId id) { return (uint8_t)(id >> 24); }

class Registry {
public:
    static constexpr size_t kMax = 2048;   // live bindings

    explicit Registry(uint8_t self = 0) : self_(self) { Clear(); }
    void  Clear();                                  // every binding gone (session start); the counter keeps going
    void  SetSelf(uint8_t peerId) { self_ = peerId; }
    NetId Allocate();                               // a fresh id owned by self; ids are never reused in a process

    bool      Bind(NetId id, uintptr_t local, uint8_t kind);   // false: 0, already bound (either side), or full
    bool      Unbind(NetId id);
    uintptr_t Local(NetId id) const;                // 0 = not bound
    NetId     IdOf(uintptr_t local) const;          // 0 = not bound
    uint8_t   KindOf(NetId id) const;               // 0 = not bound
    size_t    Count() const { return live_; }

    template <class F> void ForEach(F f) const {
        for (size_t i = 0; i < kMax; ++i) if (e_[i].used) f(e_[i].id, e_[i].local, e_[i].kind);
    }

private:
    static constexpr size_t  kIdx = 4096;           // open-addressed index tables, load <= 1/2 + tombstones
    static constexpr int16_t kEmpty = -1, kTomb = -2;
    struct Entry { NetId id; uintptr_t local; uint8_t kind; bool used; };

    static size_t HashId(NetId id) { return (size_t)((id * 2654435761u) >> 20) & (kIdx - 1); }
    static size_t HashLocal(uintptr_t p) { return (size_t)(((uint32_t)(p >> 2) * 2654435761u) >> 20) & (kIdx - 1); }
    int  FindId(NetId id) const;          // entry index or -1
    int  FindLocal(uintptr_t p) const;
    void Rebuild();                       // drop tombstones

    Entry    e_[kMax];
    int16_t  free_[kMax];
    size_t   nfree_ = 0;
    int16_t  byId_[kIdx], byLocal_[kIdx];
    size_t   live_ = 0, tombs_ = 0;
    uint8_t  self_ = 0;
    uint32_t counter_ = 0;
};

}  // namespace v2::net
