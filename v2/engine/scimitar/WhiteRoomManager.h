#pragma once

#include "BaseTypes.h"

// Manager for the white-room: the loadout/test world the player sits in between real worlds and behind
// the pause menu. A puppet must never be spawned into it, so callers resolve its world handle and
// compare it with the world they are about to touch.
//
// The manager is a heap object reached through one global pointer slot, not a static object: the
// engine's own getter (0x609670) is a single `return dword_2AC1E7C`, other engine code tests the slot
// for null before using it (0x550D80), and the destructor (0x61A020) clears it when the manager dies.
//
// +24 is a HandleContainer owned by the manager: the destructor releases it through the container
// decrement sequence, and 0x44E830 dereferences it as [0] object / [8] count. We only read that one
// field, so the rest of the object stays opaque here and is deliberately not described.
class WhiteRoomManager
{
public:
	inline static WhiteRoomManager* GetInstance()
	{
		return *(WhiteRoomManager**)0x02AC1E7C;
	}

	// Handle container for the white-room world. Null until the manager has one; the caller checks the
	// manager pointer and its extent before reading this.
	inline HandleContainer* GetWorldHandle()
	{
		return *(HandleContainer**)((char*)this + 24);
	}
};
