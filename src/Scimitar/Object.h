#pragma once

#include "ClassDescriptor.h"

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

	inline void* Delete()
	{
		void* ret = Hook::CallClassMethod<void*, Object*, char>(*vftable, this, 0);
		void* fr = Memory::Free(this);

		return ret;
	}
};