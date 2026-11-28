#include "LoadingThread.h"

// Loading-thread entry points used by the builder reload path. GetFastLoadDataStream is cdecl,
// AddLoadTask_Callback is cdecl with ten arguments, GetWaitTasks is stdcall.

int LoadingThread::GetFastLoadDataStream()
{
	return Hook::CallCdeclMethod<int>(0xA3FB90);
}

char LoadingThread::AddLoadTask_Callback(int a, int stream, int c, uint64_t id, int handlePtr,
                                         int f, int g, int h, int i, char j)
{
	return Hook::CallCdeclMethod<char, int, int, int, uint64_t, int, int, int, int, int, char>(
		0xA2A6C0, a, stream, c, id, handlePtr, f, g, h, i, j);
}

int LoadingThread::GetWaitTasks()
{
	return Hook::CallStdMethod<int>(0xA278F0);
}
