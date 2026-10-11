#pragma once

#include "BaseTypes.h"

// Async resource-load helpers (mirrors LoadingThread::LoadFile). Used only by the builder-reload
// fallback: when the player's cached EntityBuilder object has been released, we re-request it by ID and
// wait for it to repopulate. Resident in normal gameplay, so this path is rarely exercised.
class LoadingThread
{
public:
	static int  GetFastLoadDataStream();
	static char AddLoadTask_Callback(int a, int stream, int c, uint64_t id, int handlePtr,
	                                 int f, int g, int h, int i, char j);
	static int  GetWaitTasks();
};
