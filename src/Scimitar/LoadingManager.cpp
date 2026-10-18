#include "LoadingManager.h"

// Engine-wide loading state, no instance. The engine returns an int; we expose it as a bool.
bool LoadingManager::IsLoading()
{
	return Hook::CallCdeclMethod<int>(0xA26F70) != 0;
}
