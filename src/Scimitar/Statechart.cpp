#include "Statechart.h"

void Statechart::HandleTrigger(int triggerID, void* statecharCache)
{
	return Hook::CallClassMethod<void, Statechart*, int, void*>(0xA014A0, this, triggerID, statecharCache);
}