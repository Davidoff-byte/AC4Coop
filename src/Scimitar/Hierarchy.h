#pragma once

#include "..\HookMath.h"

// Transform node used by the physics hierarchy. Only the matrix and its setter are needed.
class Hierarchy
{
public:
	HookMath::Transform Matrix;

	bool SetGlobalMatrix(HookMath::Transform* transform);
};
