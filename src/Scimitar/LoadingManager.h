#pragma once

#include "BaseTypes.h"

// Static loading-state query: true while the engine is streaming or tearing down a world (it reads the
// loading thread's busy flag). Used to avoid building entities mid-load.
class LoadingManager
{
public:
	static bool IsLoading();
};
