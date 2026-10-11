#include "DuplicationManager.h"

void DuplicationManager::Begin(char a, char b)
{
	// DuplicationManager::Begin(char, char) — cdecl (fileVA 0xA2D5A0). Returns char; unused here.
	Hook::CallCdeclMethod<char, char, char>(0xA2D5A0, a, b);
}

void DuplicationManager::End()
{
	// DuplicationManager::End() — cdecl, no args (fileVA 0xA2F320).
	Hook::CallCdeclMethod<void>(0xA2F320);
}
