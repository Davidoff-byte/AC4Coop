#include "AIEntityHelper.h"

namespace AIEntityHelper
{
	AbstractEntityAI* __fastcall GetEntityAI(Entity* entity)
	{
		return Hook::CallCdeclMethod<AbstractEntityAI*, Entity*>(0x006F5F70, entity);
	}

	bool __fastcall IsInfiniteBreathMode(AbstractEntityAI* entityai)
	{
		return Hook::CallCdeclMethod<bool, AbstractEntityAI*>(0x01310560, entityai);
	}

	bool __fastcall IsGodMode(AbstractEntityAI* entityai)
	{
		return Hook::CallCdeclMethod<bool, AbstractEntityAI*>(0x01310470, entityai);
	}

	Human* __fastcall GetHuman(Entity* entity)
	{
		return Hook::CallCdeclMethod<Human*, Entity*>(0x013905A0, entity);
	}
}