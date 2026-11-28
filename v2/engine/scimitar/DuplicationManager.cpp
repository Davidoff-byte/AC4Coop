#include "DuplicationManager.h"

// Guards the engine's duplication allocator around a RowSelector::Duplicate + BuildEntityWithSelector pair.
// Both are cdecl; Begin returns a char we do not use.

void DuplicationManager::Begin(char a, char b)
{
	Hook::CallCdeclMethod<char, char, char>(0xA2D5A0, a, b);
}

void DuplicationManager::End()
{
	Hook::CallCdeclMethod<void>(0xA2F320);
}
