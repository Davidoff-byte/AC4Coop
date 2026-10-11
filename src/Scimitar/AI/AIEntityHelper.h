#pragma once

#include "AbstractEntityAI.h"
#include "Human.h"
#include "..\Entity.h"

namespace AIEntityHelper
{
	AbstractEntityAI* __fastcall GetEntityAI(Entity* entity);
	bool __fastcall IsInfiniteBreathMode(AbstractEntityAI* entityai);
	bool __fastcall IsGodMode(AbstractEntityAI* entityai);
	Human* __fastcall GetHuman(Entity* entity);
}
