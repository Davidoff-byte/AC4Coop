#include "RowSelector.h"

// Copies the player's cached row selector so a builder can produce another entity from it. Returns a fresh
// selector owned by the caller.
RowSelector* RowSelector::Duplicate(int a2, int a3)
{
	return Hook::CallClassMethod<RowSelector*, RowSelector*, int, int>(0x7A3BD0, this, a2, a3);
}
