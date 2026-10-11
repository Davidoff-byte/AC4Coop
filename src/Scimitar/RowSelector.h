#pragma once

#include "BaseTypes.h"

// A RowSelector picks which archetype/row an EntityBuilder builds. BuildEntityWithSelector consumes
// (frees) the selector it is handed, so callers Duplicate() a fresh copy first — and Duplicate needs
// an active DuplicationManager scope (see DuplicationManager::Begin/End) or it null-derefs.
class RowSelector
{
public:
	RowSelector* Duplicate(int a2, int a3);
};
