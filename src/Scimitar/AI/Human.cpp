#include "Human.h"

bool __fastcall Human::PutHoodDown(bool a1, bool a2, int a3)
{
	return Hook::CallClassMethod<bool, void**, bool, bool, int>(0x0169F260, &this->IHuman, a1, a2, a3);
}

bool __fastcall Human::PutHoodUp(bool a1, bool a2, int a3)
{
	return Hook::CallClassMethod<bool, void**, bool, bool, int>(0x0169F3C0, &this->IHuman, a1, a2, a3);
}

int* Human::GetCharacterComponent()
{
	return Hook::CallClassMethod<int*, AIActorData*>(0x005FC130, this->m_AIActorDataPtr);
}

ActionComponent* Human::GetActionComponent()
{
	return Hook::CallClassMethod<ActionComponent*, AIActorData*>(0x005FC110, this->m_AIActorDataPtr);
}

void* Human::GetContextByID(int track, int contextID)
{
	return Hook::CallClassMethod<void*, Human*, int, int>(0x016F19E0, this, track, contextID);
}

int DebugContext::Enter()
{
	return Hook::CallClassMethod<int, DebugContext*>(0x01B9D030, this);
}

int DebugContext::Exit()
{
	return Hook::CallClassMethod<int, DebugContext*>(0x01B9D080, this);
}

void DebugContext::Update(int track)
{
	return Hook::CallClassMethod<void, DebugContext*, int>(0x01BA0FC0, this, track);
}