// Pilot world queries: what is around Edward, through the engine's own queries, on the game thread.
// Static map + call recipes: bf-coop ops/evidence/pilot/world-queries.md (IDA, 2026-11-15). Every return size below was
// re-read in a second database copy (D:\ac4ida\a3 job k10) and the ray/ground calls re-decompiled (k11).
//
//   RayCast object (aligned 16): ctor sub_4C6380 (1 inline hit, 0xD0 bytes) / dtor sub_4212A0 (releases the handle ref
//   held by each hit - always call it); filter sub_620E40(ray, layer, 0) retn 8; cast sub_630600(ray, World*, from, to,
//   allHits, anyFacing) retn 14h -> hit count != 0. ray+0x08 hit array, +0x0E u16 count. Hit (64 bytes): +0x00 position,
//   +0x10 normal, +0x20 distance in metres, +0x28 type (1 ground / 4 wall / 8 ceiling), +0x2C physics object.
//   Ground probe sub_47EC30 cdecl(World*, vec4* pos, float* outZ, ws=0, halfLen) -> bool (rays pos +/- halfLen, layer 11).
//   NavWorld sub_45DD20 cdecl(World*); triangle at point sub_47A980(nav, vec4* p, meshType, 0, 0, 1.75f, 0) retn 18h;
//   triangle z sub_703090(tri, x, y) retn 8 (st0); straight-line check sub_47EA80(nav, meshType, a, b, triA, triB,
//   err[3], 0, 0.5f, 0, 0) retn 28h, 0 = clear.
//   Guidance (climbable edges): zone ctor sub_70E440 / dtor sub_70E470 (0x80 bytes: +0x10..+0x4F matrix rows, +0x60 local
//   min, +0x70 local max); report ctor sub_59FAA0 / dtor sub_59F9D0 (12 bytes, +0 items, +6 u16 count); scan
//   sub_61D270(zone, report, World*, ws=0, 1, subtypeMask, clear) retn 18h. Item 112 bytes: +0x10 A, +0x20 B,
//   +0x60 low 6 bits subtype. The game's own mask (sub_1703540) is 0x2430BA.
// UNVERIFIED until the live check: everything above is static; layer numbers and subtype names are PS3 DWARF leads.
#include "pilot.h"
#include "Hook.h"
#include "Scimitar/Entity.h"
#include "Scimitar/World.h"

#include <windows.h>
#include <cmath>
#include <cstring>

namespace Pilot {
namespace {

struct alignas(16) V4 { float x, y, z, w; };

bool Ray(World* w, const V4& from, const V4& to, int layer, float* out /* pos3, normal3, dist, type */)
{
    alignas(16) unsigned char ray[0xD0];
    Hook::CallClassMethod<void*>(0x004C6380, (void*)ray);                         // RayCast ctor, 1 inline hit
    Hook::CallClassMethod<int>(0x00620E40, (void*)ray, layer, 0);                  // collision filter
    bool hit = Hook::CallClassMethod<bool>(0x00630600, (void*)ray, (void*)w, &from, &to, false, false);
    unsigned short n = *(unsigned short*)(ray + 0x0E);
    if (hit && n) {
        const unsigned char* h = *(unsigned char* const*)(ray + 0x08);
        memcpy(out, h + 0x00, 12);
        memcpy(out + 3, h + 0x10, 12);
        out[6] = *(const float*)(h + 0x20);
        out[7] = (float)*(const int*)(h + 0x28);
    }
    Hook::CallClassMethod<void>(0x004212A0, (void*)ray);                           // dtor: releases hit handle refs
    return hit && n;
}

}  // namespace

int RunWorldCommand(PilotCtl* c, void* player)
{
    World* w = World::GetCurrentWorld();
    if (!w || !player) return -1;
    const HookMath::Transform& m = ((BaseEntity*)player)->m_GlobalMatrix;
    float* r = c->res;
    c->rcount = 0;
    switch (c->cmd) {
    case kCmdRay: {                       // f0..2 from, f3..5 to, f6 layer
        alignas(16) V4 a = { c->f[0], c->f[1], c->f[2], 0 }, b = { c->f[3], c->f[4], c->f[5], 0 };
        memset(r, 0, 8 * sizeof(float));
        c->rcount = Ray(w, a, b, (int)c->f[6], r) ? 1 : 0;
        return 0;
    }
    case kCmdGround: {                    // f0..2 point, f3 half length
        alignas(16) V4 p = { c->f[0], c->f[1], c->f[2], 0 };
        float z = 0.0f;
        bool ok = Hook::CallCdeclMethod<bool>(0x0047EC30, (void*)w, &p, &z, (void*)nullptr, c->f[3] > 0 ? c->f[3] : 3.0f);
        r[0] = z; c->rcount = ok ? 1 : 0;
        return 0;
    }
    case kCmdNavPoint: {                  // f0..2 point -> res: tri (as bits), walk z
        void* nav = Hook::CallCdeclMethod<void*>(0x0045DD20, (void*)w);
        if (!nav) return -4;
        alignas(16) V4 q = { c->f[0], c->f[1], c->f[2] + 0.5f, 0 };
        void* tri = Hook::CallClassMethod<void*>(0x0047A980, nav, &q, 0, (void*)nullptr, (void*)nullptr, 1.75f, 0);
        ((uint32_t*)r)[0] = (uint32_t)(uintptr_t)tri;
        r[1] = tri ? Hook::CallClassMethod<float>(0x00703090, tri, c->f[0], c->f[1]) : 0.0f;
        c->rcount = tri ? 1 : 0;
        return 0;
    }
    case kCmdNavLine: {                   // f0..2 a, f3..5 b -> res: code (0 = clear), err[0..2]
        void* nav = Hook::CallCdeclMethod<void*>(0x0045DD20, (void*)w);
        if (!nav) return -4;
        alignas(16) V4 a = { c->f[0], c->f[1], c->f[2], 0 }, b = { c->f[3], c->f[4], c->f[5], 0 };
        uint32_t err[3] = { 0, 0, 0 };
        // Returns a byte: the game's call site tests only al (0x015E4ADD test al,al; jz -> clear). Live run 26452 showed
        // garbage in the upper bits when read as int.
        int code = (unsigned char)Hook::CallClassMethod<char>(0x0047EA80, nav, 0, &a, &b, (void*)nullptr, (void*)nullptr, (void*)err, 0, 0.5f, 0, 0);
        ((int32_t*)r)[0] = code; ((uint32_t*)r)[1] = err[0]; ((uint32_t*)r)[2] = err[1]; ((uint32_t*)r)[3] = err[2];
        c->rcount = 1;
        return 0;
    }
    case kCmdEdges: {                     // f0 half width, f1 z min, f2 z max (relative to Edward), f3 mask (0 = game's)
        alignas(16) unsigned char zone[0x80];
        unsigned char rep[12];
        Hook::CallClassMethod<void*>(0x0070E440, (void*)zone);
        float* mz = (float*)(zone + 0x10);                                         // rows: identity rotation, Edward's position
        float rows[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, m.M41, m.M42, m.M43, 1 };
        memcpy(mz, rows, sizeof(rows));
        float hw = c->f[0] > 0 ? c->f[0] : 3.0f;
        float mn[4] = { -hw, -hw, c->f[1], 0 }, mx[4] = { hw, hw, c->f[2] > c->f[1] ? c->f[2] : 4.0f, 0 };
        memcpy(zone + 0x60, mn, 16); memcpy(zone + 0x70, mx, 16);
        Hook::CallClassMethod<void*>(0x0059FAA0, (void*)rep);
        uint32_t mask = c->f[3] > 0 ? (uint32_t)c->f[3] : 0x2430BAu;
        Hook::CallClassMethod<bool>(0x0061D270, (void*)zone, (void*)rep, (void*)w, (void*)nullptr, 1, mask, true);
        unsigned short n = *(unsigned short*)(rep + 6);
        const unsigned char* items = *(unsigned char* const*)rep;
        // Keep the kResMax/8 edges nearest to Edward (distance of the closer end point), not the first ones returned.
        uint32_t k = 0, cap = kResMax / 8;
        float d2[kResMax / 8];
        for (unsigned short i = 0; i < n; ++i) {
            const unsigned char* it = items + 112 * i;
            const float* pa = (const float*)(it + 0x10); const float* pb = (const float*)(it + 0x20);
            float da = (pa[0] - m.M41) * (pa[0] - m.M41) + (pa[1] - m.M42) * (pa[1] - m.M42) + (pa[2] - m.M43) * (pa[2] - m.M43);
            float db = (pb[0] - m.M41) * (pb[0] - m.M41) + (pb[1] - m.M42) * (pb[1] - m.M42) + (pb[2] - m.M43) * (pb[2] - m.M43);
            float d = da < db ? da : db;
            uint32_t slot = k;
            if (k == cap) {                                                        // replace the farthest kept edge
                slot = 0; for (uint32_t j = 1; j < cap; ++j) if (d2[j] > d2[slot]) slot = j;
                if (d >= d2[slot]) continue;
            } else ++k;
            float* o = r + 8 * slot;
            memcpy(o, pa, 12); memcpy(o + 3, pb, 12);                              // A, B
            o[6] = (float)(it[0x60] & 0x3F);                                       // subtype
            o[7] = (float)n;                                                       // total edges the scan returned
            d2[slot] = d;
        }
        c->rcount = k;
        Hook::CallClassMethod<void>(0x0059F9D0, (void*)rep);
        Hook::CallClassMethod<void>(0x0070E470, (void*)zone);
        return 0;
    }
    case kCmdSense: {                     // f0 range, f1 height, f2 layer, f3 rays (<=32) -> per ray: dist, type, hit z, nz
        float range = c->f[0] > 0 ? c->f[0] : 8.0f, h = c->f[1] > 0 ? c->f[1] : 1.0f;
        int layer = c->f[2] > 0 ? (int)c->f[2] : 11, n = c->f[3] > 0 ? (int)c->f[3] : 16;
        if (n > 32) n = 32;
        alignas(16) V4 a = { m.M41, m.M42, m.M43 + h, 0 };
        for (int i = 0; i < n; ++i) {
            float th = 6.2831853f * i / n;
            alignas(16) V4 b = { a.x + range * cosf(th), a.y + range * sinf(th), a.z, 0 };
            float hit[8] = { 0 };
            bool got = Ray(w, a, b, layer, hit);
            float* o = r + 4 * i;
            o[0] = got ? hit[6] : range; o[1] = got ? hit[7] : 0.0f; o[2] = got ? hit[2] : 0.0f; o[3] = got ? hit[5] : 0.0f;
        }
        c->rcount = (uint32_t)n;
        return 0;
    }
    }
    return -3;
}

}  // namespace Pilot
