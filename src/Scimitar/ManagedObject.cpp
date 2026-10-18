#include "ManagedObject.h"

// Identity lives in the handle container that refers to the object; the engine resolves it from the object.
uint64_t __thiscall ManagedObject::GetObjectID()
{
	return Hook::CallClassMethod<uint64_t, ManagedObject*>(0x009F0B50, this);
}
