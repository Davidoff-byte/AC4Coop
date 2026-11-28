#include "AIComponent.h"

// Debug/game mode switch on the assassin behaviour component. Both are plain thiscalls; the mod uses them
// to park an actor when a puppet takes over its body.

void BhvAssassin::SwitchActorToDebugMode(bool forceTeleportToGround)
{
	return Hook::CallClassMethod<void, BhvAssassin*, bool>(0x017FFDD0, this, forceTeleportToGround);
}

void BhvAssassin::SwitchActorToGameMode()
{
	return Hook::CallClassMethod<void, BhvAssassin*>(0x017FFE40, this);
}
