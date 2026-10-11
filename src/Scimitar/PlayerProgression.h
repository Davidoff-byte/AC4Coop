#pragma once

#include "BaseTypes.h"

class RowSelector;

// Globals cached by PlayerProgressionManager::LoadEntityBuilderAndSelectorsForPlayer when the local
// player spawns: the RowSelector and the EntityBuilder's handle container. We reuse both to build the
// puppet so it looks like the local player. (The builder OBJECT is released after the player spawns,
// but the handle container keeps its resource ID so it can be reloaded — see LoadingThread.)
namespace PlayerProgression
{
	inline RowSelector* GetCachedSelector()
	{
		return *(RowSelector**)0x04DEEF90;
	}

	inline HandleContainer* GetCachedBuilderHandle()
	{
		return *(HandleContainer**)0x04DEEF94;
	}
}
