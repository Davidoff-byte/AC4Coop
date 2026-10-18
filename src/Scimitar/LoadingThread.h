#pragma once

#include "BaseTypes.h"

// Async resource-load helpers. Only used by the builder-reload fallback: if the player's cached
// EntityBuilder has been released we re-request it by id and wait for it to repopulate. Resident in
// normal play, so this path is rarely taken.
class LoadingThread
{
public:
	static int  GetFastLoadDataStream();
	static char AddLoadTask_Callback(int a, int stream, int c, uint64_t id, int handlePtr,
	                                 int f, int g, int h, int i, char j);
	static int  GetWaitTasks();
};
