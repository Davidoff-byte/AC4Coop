#include "PlayerPadManager.h"

void PlayerPadManager::RegisterPlayer(Entity* entity, int pad)
{
	// PlayerPadManager::RegisterPlayer(this, entity, pad) = padMgr[pad]=entity — thiscall (fileVA 0x734500).
	Hook::CallClassMethod<void, PlayerPadManager*, Entity*, int>(0x734500, this, entity, pad);
}

PlayerPadManager* PlayerPadManager::FromWorld(World* world)
{
	if (!world)
		return nullptr;

	uintptr_t holder = *(uintptr_t*)((char*)world + 0x7F8);
	if (!holder)
		return nullptr;

	return *(PlayerPadManager**)(holder + 4);
}
