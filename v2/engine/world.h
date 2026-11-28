#pragma once
// Engine layer: world and local player (ARCHITECTURE.md R7). Receipts: decomp/units.json 0054D4D0 (GetCurrentWorld),
// 0070E810 (World::GetPlayer), 013905A0 (AIEntityHelper::GetHuman). These three are read-only getters; they are the one
// documented exception to R1 (the tick has to find the player's Human before it can run).
namespace v2::engine {

void* CurrentWorld();                 // null while loading
void* LocalPlayerEntity();            // null when no world or no player
void* LocalPlayerHuman();             // the Human component of the local player, or null
bool  IsLocalPlayerHuman(void* human);

}  // namespace v2::engine
