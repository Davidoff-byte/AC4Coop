#pragma once
// The one interpolation buffer (docs/v2/ARCHITECTURE.md R11, s7), shared by the puppet, guards, AI ships and the
// peer's ship. A port of v1's ScbSample (src/phase1.cpp:9839, [Coop] ShipInterp=2), the variant with the measured
// result: AI ships 0.25 % of frames > 15 m/s^2 at a 400 ms delay vs 5.0 % at 300 ms (docs/evidence/spawn-lib-2026-11-28.md
// s15 line 226, docs/design/ship-interp.md:57).
// Rules kept exactly:
//   - render time T = host now - delay; a = the newest sample at or before T, b = the one after it;
//   - b - a farther than 30 m apart = a step (teleport), never a glide;
//   - cubic Hermite (non-uniform Catmull-Rom tangents over the real stamps) when the neighbours prev and next exist,
//     their stamps are ordered and both neighbour segments are within 30 m; linear otherwise;
//   - rotation: nlerp along the shortest arc (v1 OwnQuatNlerp, phase1.cpp:9521); a step takes b's rotation;
//   - T before every sample: hold the oldest; T past the newest: extrapolate along the last segment for at most
//     extrapUs (only if that segment is within 30 m), rotation held.
// Pure code (explicit time), unit-tested in v2/tests. One buffer per replicated body, used by one thread.
#include <cmath>
#include <cstdint>

namespace v2::interp {

struct Pose { float pos[3]; float quat[4]; };
struct Snap { int64_t stampUs; Pose pose; };

enum class How : uint8_t { Empty, Hold, Linear, Spline, Step, Extrap };

inline void QuatNlerp(const float* a, const float* b, float t, float* out) {
    const float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    const float sg = d < 0.0f ? -1.0f : 1.0f;
    float n = 0.0f;
    for (int k = 0; k < 4; ++k) { out[k] = a[k] + (sg * b[k] - a[k]) * t; n += out[k] * out[k]; }
    n = sqrtf(n);
    if (n > 1e-6f) for (int k = 0; k < 4; ++k) out[k] /= n;
    else for (int k = 0; k < 4; ++k) out[k] = b[k];
}

inline bool Within30m(const float* a, const float* b) {
    const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    return dx * dx + dy * dy + dz * dz <= 900.0f;
}

// Cubic Hermite between a (ta) and b (tb) at fraction f; tangents m_a = (b - p) / (tb - tp), m_b = (n - a) / (tn - ta),
// scaled to the segment (v1 ShipHermite, phase1.cpp:9828).
inline void Hermite(const float* p, int64_t tp, const float* a, int64_t ta, const float* b, int64_t tb, const float* n,
                    int64_t tn, float f, float* out) {
    const float seg = (float)(double)(tb - ta);
    const float sa = seg / (float)(double)(tb - tp), sb = seg / (float)(double)(tn - ta);
    const float t2 = f * f, t3 = t2 * f;
    const float h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + f, h01 = -2 * t3 + 3 * t2, h11 = t3 - t2;
    for (int k = 0; k < 3; ++k) out[k] = h00 * a[k] + h10 * (b[k] - p[k]) * sa + h01 * b[k] + h11 * (n[k] - a[k]) * sb;
}

class Buffer {
public:
    static constexpr int kRing = 16;

    void Clear() { n_ = 0; head_ = 0; }
    int  Size() const { return n_; }
    // Samples must arrive in stamp order; an older or equal stamp is dropped (a re-sent or reordered state).
    bool Push(int64_t stampUs, const Pose& p) {
        if (n_ && stampUs <= s_[head_].stampUs) return false;
        head_ = (head_ + 1) % kRing;
        s_[head_].stampUs = stampUs; s_[head_].pose = p;
        if (n_ < kRing) ++n_;
        return true;
    }
    const Snap* Newest() const { return n_ ? &s_[head_] : nullptr; }

    How Sample(int64_t T, int64_t extrapUs, Pose& out) const {
        if (!n_) return How::Empty;
        const Snap *a = nullptr, *b = nullptr, *prev = nullptr, *next = nullptr;
        for (int k = 0; k < n_; ++k) {   // newest .. oldest
            const Snap& s = s_[(head_ - k + kRing) % kRing];
            if (s.stampUs <= T) { a = &s; if (k + 1 < n_) prev = &s_[(head_ - k - 1 + kRing) % kRing]; break; }
            next = b; b = &s;
        }
        if (!a) { out = b->pose; return How::Hold; }
        if (b) {
            const float f = (float)(double)(T - a->stampUs) / (float)(double)(b->stampUs - a->stampUs);
            if (!Within30m(a->pose.pos, b->pose.pos)) { out = b->pose; return How::Step; }
            How how = How::Linear;
            if (prev && next && b->stampUs > prev->stampUs && next->stampUs > a->stampUs &&
                Within30m(prev->pose.pos, a->pose.pos) && Within30m(b->pose.pos, next->pose.pos)) {
                Hermite(prev->pose.pos, prev->stampUs, a->pose.pos, a->stampUs, b->pose.pos, b->stampUs, next->pose.pos,
                        next->stampUs, f, out.pos);
                how = How::Spline;
            } else {
                for (int k = 0; k < 3; ++k) out.pos[k] = a->pose.pos[k] + (b->pose.pos[k] - a->pose.pos[k]) * f;
            }
            QuatNlerp(a->pose.quat, b->pose.quat, f, out.quat);
            return how;
        }
        int64_t ex = T - a->stampUs;
        if (ex > extrapUs) ex = extrapUs;
        out = a->pose;
        if (prev && a->stampUs > prev->stampUs && Within30m(prev->pose.pos, a->pose.pos)) {
            const float r = (float)(double)ex / (float)(double)(a->stampUs - prev->stampUs);
            for (int k = 0; k < 3; ++k) out.pos[k] = a->pose.pos[k] + (a->pose.pos[k] - prev->pose.pos[k]) * r;
        }
        return How::Extrap;
    }

private:
    Snap s_[kRing] = {};
    int  n_ = 0, head_ = 0;
};

}  // namespace v2::interp
