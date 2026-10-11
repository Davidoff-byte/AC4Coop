#include "Component.h"

int Component::SetEnabled(bool enabled)
{
	return Hook::CallClassMethod<int, Component*, bool>(0x007130C0, this, enabled);
}