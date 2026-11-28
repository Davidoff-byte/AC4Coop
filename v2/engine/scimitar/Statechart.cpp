#include "Statechart.h"

// Pushes one trigger into the state machine. The engine takes a 16-bit trigger id and one context pointer;
// nothing in the mod calls this today.
void Statechart::HandleTrigger(int triggerID, void* statecharCache)
{
	return Hook::CallClassMethod<void, Statechart*, int, void*>(0xA014A0, this, triggerID, statecharCache);
}
