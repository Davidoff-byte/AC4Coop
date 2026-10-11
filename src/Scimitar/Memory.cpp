#include "Memory.h"
#include "..\Hook.h"

void* __cdecl Memory::GetAllocator(size_t size, size_t alignment, void* allocator)
{
	return Hook::CallCdeclMethod<void*, size_t, size_t, void*>(0x00A085B0, size, alignment, allocator);
}

void* __cdecl Memory::GetAllocatorFromData(void* data)
{
	void* memRef = *(void**)0x02AC40F4;

	return Hook::CallClassMethod<void*, void*, void*>(0x0080AC30, memRef, data);
}

void* __cdecl Memory::Free(void* data)
{
	void* alloc = Memory::GetAllocatorFromData(data);
	void* memRef = **(void***)0x02AC40F0;

	if (alloc)
	{
		return Hook::CallClassMethod<void*, void*, void*, void*>(0x00A47EA0, memRef, data, alloc);
	}

	return 0;
}

void* __cdecl Memory::Allocate(size_t size, size_t alignment)
{
	void* defaultAlloc = *(void**)0x04DD5EE0;

	if (!defaultAlloc)
	{
		return malloc(size);
	}

	void* alloc = GetAllocator(size, alignment, defaultAlloc);

	if (alloc)
	{
		return Hook::CallCdeclMethod<void*, size_t, size_t, void*>(0x004061F0, size, alignment, alloc);
	}
	else
	{
		return malloc(size);
	}
}