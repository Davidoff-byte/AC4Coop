#pragma once
// Guard-shaped spawn probe (ops OPEN O12). Evidence: docs/evidence/guard-spawn-route-2026-11-12.md.
//
// Question: can THIS game build a guard body of a given type, so a host guard no longer has to be played by a
// borrowed local NPC? Static answer (IDA, AC4BFSP.exe): every body spawned from a SpawnEntityParams record is
// built by sub_46D8E0, which calls EntityBuilder::BuildEntityWithSelector (vtable+0x44 = 0x5FAC00) with the
// RowSelector embedded in the params at +0x3C; that selector carries its own EntityBuilder handle at +0x10.
// So a guard type is a (builder, selector) pair exactly like the player's puppet route, only it is the guard's.
//
// Everything is behind [Coop] GuardSpawnProbe (default 0 = one ini read, nothing patched):
//   1. detours BuildEntityWithSelector (0x5FAC00) to observe every build: the first time an NPC body
//      (skeleton + BhvGenericNPC) comes out of a builder id it has not seen, it duplicates that build's
//      RowSelector and keeps the copy (the guard's own selector, which holds the builder handle);
//   2. detours the entry of the per-Human update (0x16FA7D0) and, on the PLAYER's update only (the context the
//      puppet spawn already runs in), logs a census of the world's entities every 10 s and, after a delay,
//      runs N cycles of: duplicate the kept selector -> BuildEntityWithSelector -> World::AddEntity -> hold ->
//      oracle (skeleton, BhvGenericNPC, health service, same type as the source) -> World::RemoveEntity.
// Logs to <LogDir>\AC4Coop_gsp_<pid>.txt. Nothing here touches the net path or the puppet.
namespace GuardSpawnProbe {
    void Init();   // DLL_PROCESS_ATTACH, after the build gate
}
