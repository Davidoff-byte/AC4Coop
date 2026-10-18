#include "Component.h"

// The engine's own setter: an interlocked set/clear of 0x4000000 in the flags dword at this+4.
int Component::SetEnabled(bool enabled)
{
	return Hook::CallClassMethod<int, Component*, bool>(0x007130C0, this, enabled);
}
