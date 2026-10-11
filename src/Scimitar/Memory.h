#pragma once

namespace Memory
{
	void* __cdecl GetAllocator(size_t size, size_t alignment, void* allocator);
	void* __cdecl GetAllocatorFromData(void* data);
	void* __cdecl Free(void* data);
	void* __cdecl Allocate(size_t size, size_t alignment = 16);
}