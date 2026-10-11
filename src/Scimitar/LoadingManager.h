#pragma once

#include "BaseTypes.h"

// Static loading-state query. IsLoading() reads *(g_LoadingThread+432)!=0 — true while the engine is
// streaming/tearing down a world. Used to gate puppet construction so we never build mid-load.
class LoadingManager
{
public:
	static bool IsLoading();
};
