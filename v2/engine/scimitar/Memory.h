#pragma once

// Engine allocator entry points; the bodies resolve absolute addresses in Memory.cpp.
namespace Memory
{
	void* __cdecl GetAllocator(size_t size, size_t alignment, void* allocator);
	void* __cdecl GetAllocatorFromData(void* data);
	void* __cdecl Free(void* data);
	void* __cdecl Allocate(size_t size, size_t alignment = 16);
}
