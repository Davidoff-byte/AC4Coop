#include "ManagedObject.h"

uint64_t __thiscall ManagedObject::GetObjectID()
{
	return Hook::CallClassMethod<uint64_t, ManagedObject*>(0x009F0B50, this);
}