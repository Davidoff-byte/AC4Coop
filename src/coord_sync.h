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
//   field 9 helm         GcLShip+0x39A read: the uint8 itself, refused when > 1 ([Coop] HelmSync)
//                                      write: the lever sub_11FE840(GcLShip*, 1|0) - never a raw byte store,
//                                      because the lever is also what tells the controller/notifier objects
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

// Field ids carried in IntentCoordValue::field (netstate.h). 1-2 are GcLShip, 3-8 are the six battle
// objectives of one GcLBoardingBattle (+0x60..+0x74, in class-info property order).
enum CoordField : uint8_t {
    CF_HULL = 1,   // hullCurrent: GcLShip+0xD0
    CF_SAIL = 2,   // sailState:   GcLShip+0xCC (read as a dword)
    // GcLBoardingBattle: slot i is a BattleObjective* at +0x60+4*i; the synced value is its State dword at
    // +0x04 (0 Invalid, 1 InProgress, 2 Completed). ops/evidence/battle-objective-state-2026-11-21.md 3, 5.
    CF_OBJ_CREW = 3,       // +0x60 ObjectiveKillEnemyCrew
    CF_OBJ_CAPTAIN = 4,    // +0x64 ObjectiveKillCaptain
    CF_OBJ_SCOUTS = 5,     // +0x68 ObjectiveKillScouts
    CF_OBJ_OFFICERS = 6,   // +0x6C ObjectiveKillOfficers
    CF_OBJ_FLAG = 7,       // +0x70 ObjectiveDestroyFlag
    CF_OBJ_POWDER = 8,     // +0x74 ObjectiveDestroyPowderReserve
    // GcLShip again, not a battle objective ([Coop] HelmSync, default 0). The state IS the byte: there is no
    // getter and no holder field (ops/evidence/helm-state-2026-11-21.md Q2), and no per-tick writer exists, so
    // a written value is not reverted the way the hull at +0xD0 is. Carries its own epoch rule - see
    // CoordSyncApplyMsg in coord_sync.cpp: last writer wins by sequence, ties to the lower peer id.
    CF_HELM = 9,           // +0x39A CaptainAtWheel (uint8 0|1)
};

struct CoordSchema {
    uint32_t vt;                       // GcLShip = 0x026A34E8, GcLBoardingBattle = 0x026AEFD8 (*(uint32_t*)logic)
    const char* name;
    // Shipped wire key. `ent` is the owning entity (what the coord walk yields); `logic` is the GcL object
    // itself, so a class identified by the parent object's registry id (GcLBoardingBattle) can carry all 64
    // bits of it without a pointer value. kCoordKeyNone = "not a coordinated object this build syncs".
    uint64_t (*identity)(void* ent, void* logic);
    bool (*extract)(void* logic, uint8_t field, float* out);
    bool (*apply)(void* ent, uint8_t field, float v);
    const uint8_t* fields;             // field ids this class publishes, 0-terminated (0 is never a field)
};

// The row for a logic vtable, or nullptr when the class is not synced.
const CoordSchema* CoordSchemaFor(uint32_t vt);

// [Coop] CoordSync, read once (the s_x = -1 idiom) and ANDed with Net::IntentLaneOn().
bool CoordSyncOn();

// Per-frame, next to CoordEnumTick(): the HOST collects the coordinators and publishes changed fields; the
// GUEST only reports its 5 s counter line (its applies happen in CoordSyncApplyMsg, below). With [Coop]
// HelmSync on, BOTH sides also sample the local ship's helm byte every frame (CF_HELM is an own-epoch field
// and has to follow engine-made changes, not only our key presses).
void CoordSyncTick();

// One IC_COORD message, from the lane's single consumer (FightIntentDrain).
void CoordSyncApplyMsg(uint64_t wireId, uint32_t seq, const uint8_t* payload, uint8_t len);
