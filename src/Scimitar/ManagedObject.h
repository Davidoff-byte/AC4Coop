#pragma once

#include "Object.h"

class ManagedObject : public Object
{
public:
	int m_ObjectFlags;

	uint64_t __thiscall GetObjectID();
};