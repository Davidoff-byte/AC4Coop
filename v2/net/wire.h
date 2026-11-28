#pragma once
// Bounds-checked little-endian byte buffers for the wire format (net/codec). No allocation, no exceptions: a read or
// write past the end clears ok() and every later call is a no-op (reads return 0).
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace v2::net {

class Writer {
public:
    Writer(uint8_t* buf, size_t cap) : b_(buf), cap_(cap) {}
    void U8(uint8_t v) { Put(&v, 1); }
    void U16(uint16_t v) { const uint8_t t[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; Put(t, 2); }
    void U32(uint32_t v) { uint8_t t[4]; for (int i = 0; i < 4; ++i) t[i] = (uint8_t)(v >> (8 * i)); Put(t, 4); }
    void U64(uint64_t v) { uint8_t t[8]; for (int i = 0; i < 8; ++i) t[i] = (uint8_t)(v >> (8 * i)); Put(t, 8); }
    void F32(float f) { uint32_t u; memcpy(&u, &f, 4); U32(u); }
    void Bytes(const void* p, size_t n) { Put(p, n); }
    void Patch8(size_t off, uint8_t v) { if (off < n_) b_[off] = v; }   // fill in a count written earlier
    bool     ok() const { return ok_; }
    size_t   size() const { return n_; }
    size_t   room() const { return ok_ ? cap_ - n_ : 0; }
    const uint8_t* data() const { return b_; }
private:
    void Put(const void* p, size_t n) {
        if (!ok_ || n > cap_ - n_) { ok_ = false; return; }
        memcpy(b_ + n_, p, n); n_ += n;
    }
    uint8_t* b_; size_t cap_; size_t n_ = 0; bool ok_ = true;
};

class Reader {
public:
    Reader(const uint8_t* buf, size_t len) : b_(buf), len_(len) {}
    uint8_t  U8() { uint8_t v = 0; Get(&v, 1); return v; }
    uint16_t U16() { uint8_t t[2] = {}; Get(t, 2); return (uint16_t)(t[0] | (t[1] << 8)); }
    uint32_t U32() { uint8_t t[4] = {}; Get(t, 4); uint32_t v = 0; for (int i = 3; i >= 0; --i) v = (v << 8) | t[i]; return v; }
    uint64_t U64() { uint8_t t[8] = {}; Get(t, 8); uint64_t v = 0; for (int i = 7; i >= 0; --i) v = (v << 8) | t[i]; return v; }
    float    F32() { const uint32_t u = U32(); float f; memcpy(&f, &u, 4); return f; }
    void     Bytes(void* p, size_t n) { Get(p, n); }
    void     Skip(size_t n) { if (!ok_ || n > len_ - n_) { ok_ = false; return; } n_ += n; }
    bool     ok() const { return ok_; }
    size_t   left() const { return ok_ ? len_ - n_ : 0; }
private:
    void Get(void* p, size_t n) {
        if (!ok_ || n > len_ - n_) { ok_ = false; memset(p, 0, n); return; }
        memcpy(p, b_ + n_, n); n_ += n;
    }
    const uint8_t* b_; size_t len_; size_t n_ = 0; bool ok_ = true;
};

}  // namespace v2::net
