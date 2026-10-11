#include "World.h"
#include "HandleManager.h"

void World::LoadWorldAsync(uint64_t worldID, bool pauseAfterLoad, int callback, int numberOfFramesToDelay, uint64_t portalID, bool merge)
{
	return Hook::CallCdeclMethod<void, uint64_t, bool, int, int, uint64_t, bool>(0x0055C570, worldID, pauseAfterLoad, callback, numberOfFramesToDelay, portalID, merge);
}

char* World::GetWorldAlternateSourcePrefixName(uint64_t worldID)
{
	return Hook::CallCdeclMethod<char*, uint64_t>(0x718820, worldID);
}

void World::AddEntity(Entity* entity)
{
	return Hook::CallClassMethod<void, World*, Entity*>(0x0070E860, this, entity);
}

void World::RemoveEntity(Entity* entity)
{
	return Hook::CallClassMethod<void, World*, Entity*>(0x0070E880, this, entity);
}

Entity* World::GetPlayer()
{
	return Hook::CallClassMethod<Entity*, World*>(0x0070E810, this);
}

WorldComponent* World::GetWorldComponentByType(ClassDescriptor* classDescriptor)
{
	return Hook::CallClassMethod<WorldComponent*, World*, ClassDescriptor*>(0x00718B20, this, classDescriptor);
}

HandleContainer* World::GetHandle()
{
	HandleManager* handleManager = HandleManager::GetInstance();

	if (!handleManager)
	{
		return NULL;
	}

	uint64_t id = ((ManagedObject*)this)->GetObjectID();

	return handleManager->FindContainerNoLock(id, 0);
}

void World::SetSlowdownFactor(float factor)
{
	return Hook::CallClassMethod<void, World*, float>(0x711770, this, factor);
}

void World::Pause(bool pause)
{
	if (pause)
	{
		PauseWorld(false);
	}
	else
	{
		UnpauseWorld();
	}
}

void World::PauseWorld(bool forMenu)
{
	return Hook::CallClassMethod<void, World*, bool>(0x70E930, this, forMenu);
}

void World::UnpauseWorld()
{
	return Hook::CallClassMethod<void, World*>(0x70E960, this);
}