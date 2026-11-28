#include "EntityBuilder.h"
#include "Entity.h"

// Builds a real entity from a row selector. The caller wraps this in DuplicationManager::Begin/End and
// passes a duplicate of the player's cached selector.
int EntityBuilder::BuildEntityWithSelector(Entity** out, void* selector)
{
	return Hook::CallClassMethod<int, EntityBuilder*, Entity**, void*>(0x5FAC00, this, out, selector);
}
