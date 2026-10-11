#pragma once

#include "BaseTypes.h"

class Entity;
class World;

// Maps up to 8 local input pads to their player entities (slots [0..7] = Entity*). RegisterPlayer binds
// an entity to a pad; a networked puppet is a remote proxy and must NOT take a pad (that would hijack
// the real local player). GetUnusedPadID is inline-hooked elsewhere to force the puppet onto an empty
// slot; that hook keeps its raw address (it patches code, not calls it).
class PlayerPadManager
{
public:
	void RegisterPlayer(Entity* entity, int pad);

	// The pad manager hangs off the world: World+0x7F8 -> holder; PlayerPadManager* = *(holder+4).
	static PlayerPadManager* FromWorld(World* world);
};
