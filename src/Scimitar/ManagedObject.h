#pragma once

#include "Object.h"

// Engine object that carries identity and a flags dword. The object id is not stored here — it lives in
// the HandleContainer that refers to this object (see BaseTypes.h).
class ManagedObject : public Object
{
public:
	int m_ObjectFlags;

	uint64_t __thiscall GetObjectID();   // engine 0x9F0B50
};
