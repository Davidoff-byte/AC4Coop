#include "EntityBuilder.h"
#include "Entity.h"

int EntityBuilder::BuildEntityWithSelector(Entity** out, void* selector)
{
	// EntityBuilder::BuildEntityWithSelector(this, &out, selector) — thiscall (fileVA 0x5FAC00).
	return Hook::CallClassMethod<int, EntityBuilder*, Entity**, void*>(0x5FAC00, this, out, selector);
}
