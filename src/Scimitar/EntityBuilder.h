#pragma once

#include "BaseTypes.h"

class Entity;

// Builds a fully-formed Entity from a resolved builder + a (duplicated) RowSelector. The player's
// cached builder is reused so the puppet comes out looking like the local player (Edward).
class EntityBuilder
{
public:
	// out receives the built entity; selector is a duplicated RowSelector (consumed by the build).
	int BuildEntityWithSelector(Entity** out, void* selector);
};
