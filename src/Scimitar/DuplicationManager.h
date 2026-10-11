#pragma once

#include "BaseTypes.h"

// Global duplication scope. RowSelector::Duplicate (and other Duplicate paths) must run inside an active
// Begin()/End() scope — the game wraps them exactly this way in
// PlayerProgressionManager::LoadEntityBuilderAndSelectorsForPlayer. If a Duplicate faults between
// Begin and End, End MUST still run (use __try/__finally) or the scope stays unbalanced and corrupts
// every later duplication.
class DuplicationManager
{
public:
	static void Begin(char a, char b);
	static void End();
};
