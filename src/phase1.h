#pragma once
#include <cstddef>
#include "netstate.h"
// Phase 1 — spawn a second Human from the player's EntityBuilder+selector and bind it to pad 1.
// Engine entity construction is only safe on a thread the engine initialized (per-thread pool
// allocators + scheduler registration), so the spawn/drive run on the game's MAIN thread via a
// per-frame hook on World::LinkUpdateAction. The hotkey just requests; the frame hook executes.

namespace Phase1 {
    void InstallFrameHook();   // call once at DLL attach; inline-hooks World::LinkUpdateAction
    void RequestSpawn();       // hotkey handler (any thread): queue a spawn for the next frame
    void RequestDump();        // hotkey handler (any thread): queue a player/swim-chain dump next frame
    void DumpPlayerInfo();     // main-thread: print player Human* + the swim context->FSM->scope chain

    // Executed on the main thread by the frame hook:
    void* TrySpawnPuppet();    // build -> add to world -> position -> RegisterPlayer(pad 1)
    void  DespawnPuppet();     // remove the puppet from the world (peer left / disconnected)

    // Networking (Front B): capture local player state / apply a remote peer's state to the puppet.
    bool  CaptureLocalState(PlayerState& out);
    void  ApplyRemoteState(const PlayerState& in);

    // Builds "<folder containing AC4Coop.asi>\<fileName>" (the game folder), so log files never depend on a
    // machine-specific path.
    void  ModuleRelativePath(char* out, size_t cap, const char* fileName);
}
