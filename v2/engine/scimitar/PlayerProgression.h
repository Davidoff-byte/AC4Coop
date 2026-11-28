#pragma once

#include "BaseTypes.h"

class RowSelector;

// Two cache slots the engine fills when the local player spawns. Both are written only by 0x12F9C90
// (PlayerProgressionManager::LoadEntityBuilderAndSelectorsForPlayer); no other function touches them.
//
//   0x04DEEF90  RowSelector*      replaced on each spawn; the previous object is freed through the
//                                 engine allocator.
//   0x04DEEF94  HandleContainer*  the player's EntityBuilder handle; swapped with an interlocked
//                                 exchange and the previous container released.
//
// We reuse both to build the puppet so that it looks like the local player. The builder *object* may be
// released after the spawn while the handle container keeps its resource id, which is what lets us
// reload the builder by id later.
//
// Both slots are zero in the image, so either getter can return null before the first spawn. On its
// first run 0x12F9C90 seeds 0x04DEEF94 with the shared invalid container (object null) and registers an
// atexit release (0x1E39190); from then on it is never null.
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
