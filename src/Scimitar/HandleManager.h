#pragma once

#include "BaseTypes.h"

class HandleManager
{
public:
	inline static HandleManager* GetInstance()
	{
		return (HandleManager*)0x04DD5FA0;
	}

	HandleContainer* __thiscall CreateReference(uint64_t ID, void* memoryAddress);
	HandleContainer* __thiscall CreateHandle(uint64_t ID, void* memoryAddress);
	HandleContainer* __thiscall FindContainerNoLock(uint64_t ID, int memoryAddress);
	HandleContainer* __thiscall FindContainer(uint64_t ID, void* memoryAddress);
	void __thiscall DestroyHandleContainer(HandleContainer* container);
	void __thiscall OnHardRefCountToZero(HandleContainer* container);
	static void DecSoftRefCount(HandleContainer* container);
	static void DecHardRefCount(HandleContainer* container);
};
