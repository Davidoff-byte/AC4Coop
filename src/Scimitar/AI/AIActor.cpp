#include "AIActor.h"

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

int AIActor::TransitToContext(int contextID)
{
	// AIActor::TransitToContext(this, id) — thiscall (fileVA 0x52DF90): exit current + enter id on the
	// active track (this[0x100], expected to be the main AI track during locomotion).
	return Hook::CallClassMethod<int, AIActor*, int>(0x0052DF90, this, contextID);
}