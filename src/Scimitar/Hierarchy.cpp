#include "Hierarchy.h"
#include "..\Hook.h"

// Moves a hierarchy node. The two trailing arguments are the engine's own defaults (0, 0).
bool Hierarchy::SetGlobalMatrix(HookMath::Transform* transform)
{
	return Hook::CallClassMethod<bool, Hierarchy*, HookMath::Transform*, int, int>(0x006BC040, this, transform, 0, 0);
}
