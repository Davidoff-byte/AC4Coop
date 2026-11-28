#include "test.h"
#include "../core/interp.h"

#include <cmath>

using namespace v2::interp;

namespace {
Pose At(float x, float y = 0, float z = 0) { Pose p{ { x, y, z }, { 0, 0, 0, 1 } }; return p; }
bool Near(float a, float b, float eps = 1e-3f) { return fabsf(a - b) <= eps; }
}  // namespace

TEST(interp_empty_hold_step) {
    Buffer b;
    Pose o;
    CHECK(b.Sample(0, 0, o) == How::Empty);
    b.Push(1000, At(1)); b.Push(2000, At(2));
    CHECK(b.Sample(500, 0, o) == How::Hold && Near(o.pos[0], 1));          // before every sample: the oldest
    CHECK(b.Sample(1500, 0, o) == How::Linear && Near(o.pos[0], 1.5f));    // no neighbours: linear
    CHECK(!b.Push(2000, At(9)) && !b.Push(1500, At(9)));                  // stale stamps dropped
    b.Push(3000, At(100));                                                // 98 m: a step
    CHECK(b.Sample(2500, 0, o) == How::Step && Near(o.pos[0], 100));
}

TEST(interp_spline_exact_on_constant_velocity) {
    // Samples of uniform motion at uneven stamps: Catmull-Rom with real-stamp tangents reproduces the line exactly.
    Buffer b;
    const int64_t t[] = { 0, 33000, 50000, 100000, 116000, 150000 };
    for (int64_t s : t) b.Push(s, At(s * 1e-5f * 3.0f, -(float)s * 1e-5f));   // 3 m/s x, -1 m/s y... scaled per us
    Pose o;
    bool all = true;
    for (int64_t T = 33000; T <= 116000; T += 1000) {
        const How h = b.Sample(T, 0, o);
        if (T > 33000 && T < 116000 && h != How::Spline) all = false;
        if (!Near(o.pos[0], T * 1e-5f * 3.0f, 2e-3f) || !Near(o.pos[1], -(float)T * 1e-5f, 2e-3f)) all = false;
    }
    CHECK(all);
}

TEST(interp_spline_velocity_continuous) {
    // A turning path: the spline's velocity has no jump at a sample (the kink a linear path has).
    Buffer b;
    for (int i = 0; i < 10; ++i) { const float a = i * 0.3f; b.Push(i * 100000, At(10 * cosf(a), 10 * sinf(a))); }
    auto vel = [&](int64_t T) { Pose p0, p1; b.Sample(T - 500, 0, p0); b.Sample(T + 500, 0, p1); return (p1.pos[0] - p0.pos[0]) / 1000.0f; };
    const int64_t s = 500000;   // a sample
    const float before = vel(s - 2000), after = vel(s + 2000);
    CHECK(fabsf(after - before) < 1e-4f);
}

TEST(interp_extrapolation_bounded) {
    Buffer b;
    b.Push(0, At(0)); b.Push(100000, At(1));      // 10 m/s
    Pose o;
    CHECK(b.Sample(150000, 200000, o) == How::Extrap && Near(o.pos[0], 1.5f));
    CHECK(b.Sample(900000, 200000, o) == How::Extrap && Near(o.pos[0], 3.0f));   // capped at 200 ms
    Buffer c;
    c.Push(0, At(0)); c.Push(100000, At(50));     // last segment is a step: hold, never fling
    CHECK(c.Sample(150000, 200000, o) == How::Extrap && Near(o.pos[0], 50));
}

TEST(interp_quat_shortest_arc) {
    Buffer b;
    Pose p0 = At(0), p1 = At(1);
    p1.quat[3] = -1;                              // the same rotation, other sign: no 360 spin
    b.Push(0, p0); b.Push(100000, p1);
    Pose o;
    b.Sample(50000, 0, o);
    CHECK(Near(fabsf(o.quat[3]), 1.0f));
    const float s2 = sqrtf(0.5f);
    Pose q0 = At(0), q1 = At(1);
    q1.quat[1] = s2; q1.quat[3] = s2;              // 90 deg about y
    Buffer c; c.Push(0, q0); c.Push(100000, q1);
    c.Sample(50000, 0, o);
    const float n = sqrtf(o.quat[0] * o.quat[0] + o.quat[1] * o.quat[1] + o.quat[2] * o.quat[2] + o.quat[3] * o.quat[3]);
    CHECK(Near(n, 1.0f) && Near(o.quat[1], sinf(0.3926991f), 1e-2f));   // ~45 deg (nlerp, not slerp: 1e-2)
}

TEST(interp_ring_wraps) {
    Buffer b;
    for (int i = 0; i < 40; ++i) b.Push(i * 1000, At((float)i));
    CHECK(b.Size() == Buffer::kRing && b.Newest()->stampUs == 39000);
    Pose o;
    CHECK(b.Sample(30500, 0, o) == How::Spline && Near(o.pos[0], 30.5f));
    CHECK(b.Sample(1000, 0, o) == How::Hold && Near(o.pos[0], 24));      // older than the ring: its oldest
}
