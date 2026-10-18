#pragma once

#include "BaseTypes.h"

// Chooses the archetype row an EntityBuilder instantiates. The builder consumes the selector it is
// handed, so callers must pass a fresh Duplicate() — and Duplicate requires an active duplication scope
// (DuplicationManager::Begin/End), otherwise it dereferences null.
class RowSelector
{
public:
	RowSelector* Duplicate(int a2, int a3);
};
