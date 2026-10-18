#include "AIActor.h"

// Context/track control. Each is a direct thiscall into the engine; the actor is only ever held as an
// opaque pointer by our code.

void* AIActor::EnterContext(int contextID)
{
	return Hook::CallClassMethod<void*, AIActor*, int>(0x0052DFF0, this, contextID);
}

void AIActor::ExitCurrentContext()
{
	return Hook::CallClassMethod<void, AIActor*>(0x0052C330, this);
}

int AIActor::GetCurrentContextID(int trackID)
{
	return Hook::CallClassMethod<int, AIActor*, int>(0x0052C2E0, this, trackID);
}

// Leaves whatever the active track is playing and enters the new context in its place.
int AIActor::TransitToContext(int contextID)
{
	return Hook::CallClassMethod<int, AIActor*, int>(0x0052DF90, this, contextID);
}
