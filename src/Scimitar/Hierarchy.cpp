#include "Hierarchy.h"
#include "..\Hook.h"

bool Hierarchy::SetGlobalMatrix(HookMath::Transform* transform)
{
	return Hook::CallClassMethod<bool, Hierarchy*, HookMath::Transform*, int, int>(0x006BC040, this, transform, 0, 0);
}