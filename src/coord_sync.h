#pragma once

// Coordinator sync ([Coop] CoordSync, default 0): the first write lane on top of the read-only coordinator
// enumerator (coord_enum.h). One table row per GcL* logic class says how that class is identified on the
// wire and which of its fields can be read and written, so a new class is a new row - not new lane code.
//
// GcLShip (logic vtable 0x026A34E8, 9 coordinators on the 2026-11-21 live census):
//   field 1 hullCurrent  GcLShip+0xD0  read: slot 0x1B4 getter (the path ReadShipHullHP uses)
//                                      write: raw +0xD0 (WriteShipHullHPRaw)
//   field 2 sailState    GcLShip+0xCC  read: plain dword, refused when >= 4 (SailState::Count)
//                                      write: the engine setter 0x01220960 (SailSetViaEngine)
//
// Identity: kCoordKeyLocal (0) is THIS machine's local ship on both sides - the host's Jackdaw and the
// guest's Jackdaw are the same gameplay ship (docs/design/sail-memento.md 7, the same assumption IC_SHIP
// already falls back on). A ShipMirror proxy yields the host's entity id of the ship it duplicates, and
// kCoordKeyNone means "not a coordinated object this build syncs". kCoordKeyLocal and kCoordKeyNone are
// therefore distinct: 0 is a legal wire key, never a sentinel.
//
// Every callback runs on the game thread only. The host publishes; the guest applies from the intent
// lane's single consumer (FightIntentDrain in phase1.cpp), exactly like IC_SHIP. coord_sync.cpp holds no
// engine address of its own - the field routes come from phase1.cpp, where the one copy of each lives.

#include <cstdint>

// Wire key of the local ship (the object both players drive). Legal on the wire; see identity().
static const uint64_t kCoordKeyLocal = 0;
// "Not a coordinated object this build syncs": identity() returns this; the host then skips the entry.
static const uint64_t kCoordKeyNone = ~(uint64_t)0;

// Field ids carried in IntentCoordValue::field (netstate.h).
enum CoordField : uint8_t {
    CF_HULL = 1,   // hullCurrent: GcLShip+0xD0
    CF_SAIL = 2,   // sailState:   GcLShip+0xCC (read as a dword)
};

struct CoordSchema {
    uint32_t vt;                       // GcLShip = 0x026A34E8 (*(uint32_t*)logic)
    const char* name;
    uint64_t (*identity)(void* ent);   // local ship -> 0; mirror proxy -> hostId; else kCoordKeyNone (not synced)
    bool (*extract)(void* logic, uint8_t field, float* out);
    bool (*apply)(void* ent, uint8_t field, float v);
};

// The row for a logic vtable, or nullptr when the class is not synced.
const CoordSchema* CoordSchemaFor(uint32_t vt);

// [Coop] CoordSync, read once (the s_x = -1 idiom) and ANDed with Net::IntentLaneOn().
bool CoordSyncOn();

// Per-frame, next to CoordEnumTick(): the HOST collects the coordinators and publishes changed fields; the
// GUEST only reports its 5 s counter line (its applies happen in CoordSyncApplyMsg, below).
void CoordSyncTick();

// One IC_COORD message, from the lane's single consumer (FightIntentDrain).
void CoordSyncApplyMsg(uint64_t wireId, uint32_t seq, const uint8_t* payload, uint8_t len);
