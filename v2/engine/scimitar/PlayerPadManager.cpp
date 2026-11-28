#include "PlayerPadManager.h"

// Per-pad slot table. RegisterPlayer overwrites the slot for that pad index.
void PlayerPadManager::RegisterPlayer(Entity* entity, int pad)
{
	Hook::CallClassMethod<void, PlayerPadManager*, Entity*, int>(0x734500, this, entity, pad);
}

// The manager is not a world member directly: the world holds a holder pointer at +0x7F8 and the manager
// sits at holder+4.
PlayerPadManager* PlayerPadManager::FromWorld(World* world)
{
	if (!world)
		return nullptr;

	uintptr_t holder = *(uintptr_t*)((char*)world + 0x7F8);
	if (!holder)
		return nullptr;

	return *(PlayerPadManager**)(holder + 4);
}
