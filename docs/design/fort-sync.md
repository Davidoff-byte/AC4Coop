# Fort sync — design (2026-11-20)

Status: **design only, no code.** Forts are the newest feature and the least verified: the damage path is
found (`ops/evidence/fort-damage-path-2-2026-11-20.md`, verdict **B**), but the hop from "damage" to
"tower disabled → phase advance → victory" is `[UNPROVEN]` and is the first thing to test.

---

## 1. The object model (reflection, verified)

```
GcLNavalFort          classinfo 0x02A18520  size 0x3D8   — the fort component on a fort entity
  +0x118 SmallArray<Handle> TowersAlive        +0xC0 SmallArray<Handle> DestroyedFortEntities
  +0x74  U32 FortPhaseCounter                  +0x120 CurrentFortPhase
GcLNavalTowerACGA     classinfo 0x02A020C8  size 0x4F0   — THE RUNTIME tower class
  +0x16C Float CurrentHealth                   +0x2E3 Bool IsFortTowerDisabled
FortTowerManager      classinfo 0x02A911C0  size 0xF0 base Component
  +0x2C  SmallArray<ObjectPtr> RuntimeTowers (ACGA)     +0x81 Bool PlayerShipInFortRange
```
`GcLNavalTower` (`0x029A7C00`, health `+0xC0`) is a **sibling**, not a base — runtime instances are ACGA.

## 2. The engine path (verified)

```
0x17E5DF0  __thiscall(ecx = GcLNavalTowerACGA*, float damage)   = ApplyDamage
0x17DEA8E  health -= damage, clamped to [settings+0xD0, settings+0xCC]
0x17E987A  in sub_17E97D0: *(u8*)(tower+0x2E3) = 1, posts "FIGHT_FORT_STOP"   — the single setter
0x18A2D00  fort message handler case 8: TowersAlive RemoveAt, --Fort+0x10C,
           CurrentFortPhase reload, ++*(Fort+0x74); remaining==0 posts the victory message
```

## 3. The design — a memento per tower

Fort sync is **not** a fort-level scalar. The bar is N towers; the victory is a consequence of the last
tower. So the sync unit is `(fort, towerIndex)` and the payload is that tower's state.

| item | value |
|---|---|
| channel | `IC_FORT` (new id; 11 is `IC_SHIP`) |
| payload | `{ u8 towerIndex; u8 flags; f32 health }` (flags bit0 = disabled) |
| identity | fort matched by **position** (forts are static, runtime objIds differ per machine); tower by index within `TowersAlive`, falling back to relative position |
| authority | host only publishes; a guest-originated fort message is ignored and counted |
| freshness | lane `chanSeq` + `hostMs`, as the sail memento does |
| apply | **health drop → `0x17E5DF0(tower, delta)`**, letting the engine drive the clamp, the disabled flag, the phase counter and the victory. Never write `+0x16C` or `+0x2E3` directly |
| repair / equal | nothing (no repair path found; a higher value is ignored and counted) |

Rationale: applying damage through the engine means the bar, `IsFortTowerDisabled`, the
`TowersAlive → DestroyedFortEntities` move and the `FortPhaseCounter` all happen by the game's own code.
Writing the fields would fake the bar and skip the phase/`CompleteMsg`.

## 4. The oracle, written before the code

**F1 — does the engine cascade?** With the probe holding a live tower:
1. read `hp0`, `disabled0`, `phase0`, `alive0`;
2. call `0x17E5DF0(tower, hp0 + 1.0f)` (lethal) **once**;
3. PASS if within 2 s: `hp` <= 0, `disabled` = 1, `TowersAlive` count −1, `phase` = phase0+1 (or the
   fort's victory message fires when it was the last tower).
   FAIL if `hp` drops but `disabled` stays 0 — that means the disable is a separate hop (the unproven
   vtable `0x026F6348` caller) and the sync must call it explicitly.

**F2 — cross-machine (only after F1 PASSes):** host damages a tower; guest's tower health falls within
1 s and the guest's `TowersAlive`/phase match. CONTROL: `FortSync=0` → guest unchanged.

**F3 — guest initiator:** guest damages; host sees it. This needs the reverse direction (guest → host
damage request), which is S9-style authority, not this slice.

## 5. Non-goals (this slice)

Repair, fort "attack/defend" mission state, the mission-side victory consumer (`UNPROVEN`, may live in
a manager or MP exe), and matching forts across **different saves**.

## 6. Dependencies

- the fort enumeration probe (`[Coop] FortProbe`) — without it we cannot even list the towers;
- F1 must pass before any wire work: if the engine does not cascade, the sync needs the disable hop as a
  second call, and the design changes.
