#include "LoadingThread.h"

int LoadingThread::GetFastLoadDataStream()
{
	// GetFastLoadDataStream() — cdecl, no args (fileVA 0xA3FB90).
	return Hook::CallCdeclMethod<int>(0xA3FB90);
}

char LoadingThread::AddLoadTask_Callback(int a, int stream, int c, uint64_t id, int handlePtr,
                                         int f, int g, int h, int i, char j)
{
	// LoadingThread::AddLoadTask_Callback(...) — cdecl (fileVA 0xA2A6C0).
	return Hook::CallCdeclMethod<char, int, int, int, uint64_t, int, int, int, int, int, char>(
		0xA2A6C0, a, stream, c, id, handlePtr, f, g, h, i, j);
}

int LoadingThread::GetWaitTasks()
{
	// LoadingThread::GetWaitTasks() — stdcall, no args (fileVA 0xA278F0).
	return Hook::CallStdMethod<int>(0xA278F0);
}
