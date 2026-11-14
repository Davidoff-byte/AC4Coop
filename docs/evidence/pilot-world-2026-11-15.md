# [Pilot] world queries: live check, 2026-11-15

Build c0daf16 (tree clean), asi f3d07c6f7fdd975a53b248f3fa870dd65cfdf66225d8ed907b0f82deb23c8abd, run pid 26452, Havana
save, Edward at (76.00, -74.00, 1.78). Static source of every call: bf-coop ops/evidence/pilot/world-queries.md; return
sizes re-read in a second idb (D:\ac4ida\a3 job k10). Verdict: **solo-tested** for ray, ground, navmesh point, ray fan,
edge scan; navmesh line fixed after this run (see below), not re-run yet.

| query | command | result | PASS rule (from world-queries.md) |
|---|---|---|---|
| ground probe sub_47EC30 | `pilot.py ground` | z = 1.780 at Edward's feet (foot z 1.78) | within 0.2 m: **PASS** |
| ray sub_630600 (layer 11) | `ray 76 -74 3.8 76 -74 -1.2` | hit (76.00,-74.00,1.78), normal (-0.09,0.02,1.00), dist 2.02 m, type 1 ground | type 1 and z within 0.2 m: **PASS** |
| navmesh point sub_47A980 + sub_703090 | `nav` | triangle 3DEA2354, walk z 1.886 | within 0.2 m: **PASS** (0.11) |
| ray fan (16 x sub_630600) | `sense --range 12` | walls at 2.92 m (270°), 6.0-7.0 m (112°, 135°, 202°, 225°), others open | plausible against the market stalls; not cross-checked with a screenshot |
| guidance scan sub_61D270 | `edges --r 6 --zmin -1 --zmax 6` | 32 (result cap) LedgeGrab edges, +0.7 to +3.1 m above the feet, around x = 70-71 and (77,-79) | items of subtype 1 1.5-3 m up: **PASS** (e.g. +2.93..+3.11 at (71.1,-70.6)) |
| navmesh line sub_47EA80 | `navline 70.9 -70.3` / `navline 76 -60` | `code=224777984 err=0` / `code=1 err=2` | **defect found**: the return is a byte - the game's call site tests only `al` (0x015E4ADD `test al,al`). Read as int it carried garbage. Fixed in the next commit (`(unsigned char)` of a `char` return): first = 0 (clear, the walk done earlier), second = blocked with err 2 (PS3 name RayCollision, lead) |

Follow-ups in the same commit: the edge scan now keeps the 32 edges nearest to Edward (was: the first 32 the engine
returned) and reports the total.
