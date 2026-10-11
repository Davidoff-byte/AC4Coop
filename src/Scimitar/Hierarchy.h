#pragma once

#include "..\HookMath.h"

class Hierarchy
{
public:
	HookMath::Transform Matrix;

	bool SetGlobalMatrix(HookMath::Transform* transform);
};
