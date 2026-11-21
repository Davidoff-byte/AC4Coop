#pragma once

// Coordinator enumerator (read-only foundation for coordinating every gameplay object).
//
// Every coordinated gameplay object (ships, navigation, forts, ...) is reached through the same chain, which is
// given by reflection and used here only as offsets - nothing below calls or writes an engine address:
//
//   entity +0x60[ ]       component array, count (u16) at +0x66
//     GameplayCoordinatorComponent   classinfo 0x027AFEF0, size 0x80
//       +0x60 ObjectPtr Coordinator  -> AbstractGameplayCoordinator  classinfo 0x0276FE08
//         +0x64 ObjectPtr GameplayLogic -> GcL*  (identified by its vtable)
//
// Measured live: 83 coordinators over 4096 entities, 12 distinct logic classes (GcLShip n=9, GcLShipNavigation
// n=35). This module is the foundation for the lanes that will sync those objects; it only reads.

#include <cstdint>

struct CoordRef
{
    void*    ent;      // owning entity
    uint64_t objId;    // stable id (HandleContainer::GetObjectID of the entity), 0 if unknown
    void*    comp;     // the GameplayCoordinatorComponent
    void*    coord;    // comp+0x60
    void*    logic;    // coord+0x64 (the GcL* object)
    uint32_t vt;       // *(uint32_t*)logic
};

// Walk World::m_EntityManager.m_Entities, fill up to max entries, return the count written.
// Entries whose chain is unreadable, or whose GameplayLogic slot is null, are skipped: every dereference is
// Readable()-guarded and SEH-wrapped, so a stale slot can never fault the calling frame.
int CoordCollect(CoordRef* out, int max);

// Re-walk and return true with the first entry whose logic vtable == vt and whose objId matches. objId is the
// identity the sync lanes use, so objId 0 never matches (matching by vtable alone is not allowed).
bool CoordFindByObjId(uint64_t objId, uint32_t vt, CoordRef* out);

// [Coop] CoordEnum (default 0, read-only): census probe over CoordCollect. With the flag 0 nothing runs and no
// game memory is read.
void CoordEnumTick();
