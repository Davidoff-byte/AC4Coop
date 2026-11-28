#pragma once

#include "BaseTypes.h"

class Entity;
class World;

// Maps up to 8 local pads to their player entities (slots are Entity*). A networked puppet is a remote
// proxy and must NOT be bound to a pad, or it hijacks the local player's input.
class PlayerPadManager
{
public:
	void RegisterPlayer(Entity* entity, int pad);   // engine 0x734500 = padMgr[pad] = entity

	// Reached through the world: World+0x7F8 -> holder; PlayerPadManager* = *(holder+4).
	static PlayerPadManager* FromWorld(World* world);
};
