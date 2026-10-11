#include "RowSelector.h"

RowSelector* RowSelector::Duplicate(int a2, int a3)
{
	// RowSelector::Duplicate(this, 0, 0) — thiscall (fileVA 0x7A3BD0). Returns a fresh RowSelector*.
	return Hook::CallClassMethod<RowSelector*, RowSelector*, int, int>(0x7A3BD0, this, a2, a3);
}
