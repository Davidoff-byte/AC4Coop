#pragma once

#include "ClassDescriptor.h"

// Root of every engine object: a vtable pointer and nothing else. The base is 4 bytes, so the first
// real field of any derived class sits at +4 (ManagedObject's flags dword, for example).
//
// The three helpers are virtual-slot calls through that table. Only Delete is used by this project
// (from the handle release path in BaseTypes.h); the other two are kept because they describe slots
// callers may need and cost nothing when unused.
class Object
{
public:
	int* vftable;

	inline ClassDescriptor* GetClassDescriptor()
	{
		if (vftable[5])
		{
			return Hook::CallCdeclMethod<ClassDescriptor*>(vftable[5]);
		}

		return nullptr;
	}

	inline int FastLoad(void* fastLoadSerializer)
	{
		if (vftable[2])
		{
			return Hook::CallClassMethod<int, Object*, void*>(vftable[2], this, fastLoadSerializer);
		}

		return 0;
	}

	// Slot 0 is the deleting destructor form; the 0 argument means "destroy but do not free", and the
	// memory then goes back through the engine allocator.
	inline void* Delete()
	{
		void* ret = Hook::CallClassMethod<void*, Object*, char>(*vftable, this, 0);
		void* fr = Memory::Free(this);

		return ret;
	}
};
