#include "AIComponent.h"

void BhvAssassin::SwitchActorToDebugMode(bool forceTeleportToGround)
{
	return Hook::CallClassMethod<void, BhvAssassin*, bool>(0x017FFDD0, this, forceTeleportToGround);
}

void BhvAssassin::SwitchActorToGameMode()
{
	return Hook::CallClassMethod<void, BhvAssassin*>(0x017FFE40, this);
}