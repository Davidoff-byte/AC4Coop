#pragma once

#include "BaseTypes.h"

// The white-room is the loadout/test world the player sits in between real worlds (and behind the pause
// menu). We must never spawn a puppet into it. The manager is a global; +24 is a handle container to
// the white-room World. Callers resolve that handle and compare it to the active world.
class WhiteRoomManager
{
public:
	inline static WhiteRoomManager* GetInstance()
	{
		return *(WhiteRoomManager**)0x02AC1E7C;
	}

	// Handle container (obj/refcounts) for the white-room world; resolve with the caller's mask helper.
	inline HandleContainer* GetWorldHandle()
	{
		return *(HandleContainer**)((char*)this + 24);
	}
};
