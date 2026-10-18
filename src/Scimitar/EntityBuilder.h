#pragma once

#include "BaseTypes.h"

class Entity;

// Produces a fully built Entity from a resolved builder plus a duplicated RowSelector. Reusing the
// player's cached builder is what makes the puppet look like the local character.
class EntityBuilder
{
public:
	// `out` receives the built entity; `selector` is a duplicated RowSelector and is consumed by the build.
	int BuildEntityWithSelector(Entity** out, void* selector);
};
