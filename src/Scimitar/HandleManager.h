#pragma once

#include "BaseTypes.h"

// Engine-wide handle table.
//
// There is exactly one instance: a static object at 0x04DD5FA0. The game has no getter for it -
// callers load that address straight into ecx and call the methods below directly (see 0x00407993
// for a CreateHandle call, 0x004079D4 for DestroyHandleContainer). Nothing here is virtual and we
// never read the table's own fields, so the class is declared as an opaque `this` type.
//
// Table entries are 20 bytes: object pointer at +0, a reference counter at +4, another at +8, and
// the object id at +12. CreateReference/CreateHandle are the only writers of the counters.
class HandleManager
{
public:
	// Returns the address of the static instance itself, not of a pointer holding it.
	inline static HandleManager* GetInstance()
	{
		return (HandleManager*)0x04DD5FA0;
	}

	// Find-or-create with the table lock already held. Returns the entry for the id; when the id is
	// unknown it takes a free slot out of the table's pool, registers the id and returns that.
	HandleContainer* __thiscall CreateReference(uint64_t ID, void* memoryAddress);	// 0xA0BE50, bumps +8
	HandleContainer* __thiscall CreateHandle(uint64_t ID, void* memoryAddress);		// 0xA0BDE0, bumps +4

	// Plain lookups. NoLock assumes the caller holds the lock and creates on a miss; the other takes
	// the lock itself and only reports what is already registered.
	HandleContainer* __thiscall FindContainerNoLock(uint64_t ID, int memoryAddress);	// 0xA0BBF0
	HandleContainer* __thiscall FindContainer(uint64_t ID, void* memoryAddress);		// 0xA0BB40

	// Release an entry back to the table / run the teardown for the last hard reference.
	void __thiscall DestroyHandleContainer(HandleContainer* container);		// 0xA094D0
	void __thiscall OnHardRefCountToZero(HandleContainer* container);		// 0xA0C160

	// Local helpers; the engine has no counterpart, they compose the two calls above.
	static void DecSoftRefCount(HandleContainer* container);
	static void DecHardRefCount(HandleContainer* container);
};
