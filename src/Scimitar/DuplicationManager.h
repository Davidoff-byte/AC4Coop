#pragma once

#include "BaseTypes.h"

// Global duplication scope. RowSelector::Duplicate (and every other Duplicate path) must run inside an
// active Begin()/End() pair. If a Duplicate faults in between, End() must still run (__try/__finally) or
// the scope stays unbalanced and corrupts later duplications.
class DuplicationManager
{
public:
	static void Begin(char a, char b);
	static void End();
};
