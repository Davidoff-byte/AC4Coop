#include "LoadingManager.h"

bool LoadingManager::IsLoading()
{
	// LoadingManager::IsLoading() — cdecl, no args (fileVA 0xA26F70). Returns int; normalize to bool.
	return Hook::CallCdeclMethod<int>(0xA26F70) != 0;
}
