#include "registry.h"

namespace v2::net {

void Registry::Clear() {
    for (size_t i = 0; i < kMax; ++i) { e_[i] = Entry{}; free_[i] = (int16_t)(kMax - 1 - i); }
    nfree_ = kMax;
    for (size_t i = 0; i < kIdx; ++i) byId_[i] = byLocal_[i] = kEmpty;
    live_ = tombs_ = 0;
}

NetId Registry::Allocate() {
    counter_ = (counter_ + 1) & 0xFFFFFFu;
    if (!counter_) counter_ = 1;   // 2^24 ids per owner per process; wrap skips 0
    return MakeNetId(self_, counter_);
}

int Registry::FindId(NetId id) const {
    for (size_t h = HashId(id), n = 0; n < kIdx; ++n, h = (h + 1) & (kIdx - 1)) {
        const int16_t s = byId_[h];
        if (s == kEmpty) return -1;
        if (s >= 0 && e_[s].id == id) return s;
    }
    return -1;
}

int Registry::FindLocal(uintptr_t p) const {
    for (size_t h = HashLocal(p), n = 0; n < kIdx; ++n, h = (h + 1) & (kIdx - 1)) {
        const int16_t s = byLocal_[h];
        if (s == kEmpty) return -1;
        if (s >= 0 && e_[s].local == p) return s;
    }
    return -1;
}

void Registry::Rebuild() {
    for (size_t i = 0; i < kIdx; ++i) byId_[i] = byLocal_[i] = kEmpty;
    for (size_t i = 0; i < kMax; ++i) {
        if (!e_[i].used) continue;
        size_t h = HashId(e_[i].id);
        while (byId_[h] != kEmpty) h = (h + 1) & (kIdx - 1);
        byId_[h] = (int16_t)i;
        h = HashLocal(e_[i].local);
        while (byLocal_[h] != kEmpty) h = (h + 1) & (kIdx - 1);
        byLocal_[h] = (int16_t)i;
    }
    tombs_ = 0;
}

bool Registry::Bind(NetId id, uintptr_t local, uint8_t kind) {
    if (!id || !local || !nfree_ || FindId(id) >= 0 || FindLocal(local) >= 0) return false;
    if (tombs_ > kIdx / 4) Rebuild();
    const int16_t s = free_[--nfree_];
    e_[s] = Entry{ id, local, kind, true };
    size_t h = HashId(id);
    while (byId_[h] >= 0) h = (h + 1) & (kIdx - 1);      // first empty or tombstone
    if (byId_[h] == kTomb) --tombs_;
    byId_[h] = s;
    h = HashLocal(local);
    while (byLocal_[h] >= 0) h = (h + 1) & (kIdx - 1);
    if (byLocal_[h] == kTomb) --tombs_;
    byLocal_[h] = s;
    ++live_;
    return true;
}

bool Registry::Unbind(NetId id) {
    const int s = FindId(id);
    if (s < 0) return false;
    for (size_t h = HashId(id);; h = (h + 1) & (kIdx - 1)) if (byId_[h] == s) { byId_[h] = kTomb; ++tombs_; break; }
    for (size_t h = HashLocal(e_[s].local);; h = (h + 1) & (kIdx - 1)) if (byLocal_[h] == s) { byLocal_[h] = kTomb; ++tombs_; break; }
    e_[s] = Entry{};
    free_[nfree_++] = (int16_t)s;
    --live_;
    return true;
}

uintptr_t Registry::Local(NetId id) const { const int s = FindId(id); return s >= 0 ? e_[s].local : 0; }
NetId     Registry::IdOf(uintptr_t p) const { const int s = FindLocal(p); return s >= 0 ? e_[s].id : 0; }
uint8_t   Registry::KindOf(NetId id) const { const int s = FindId(id); return s >= 0 ? e_[s].kind : 0; }

}  // namespace v2::net
