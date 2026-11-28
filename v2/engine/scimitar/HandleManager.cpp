#include "HandleManager.h"

HandleContainer* __thiscall HandleManager::CreateReference(uint64_t ID, void* memoryAddress)
{
	return Hook::CallClassMethod<HandleContainer*, HandleManager*, uint64_t, void*>(0xA0BE50, this, ID, memoryAddress);
}

HandleContainer* __thiscall HandleManager::CreateHandle(uint64_t ID, void* memoryAddress)
{
	return Hook::CallClassMethod<HandleContainer*, HandleManager*, uint64_t, void*>(0xA0BDE0, this, ID, memoryAddress);
}

HandleContainer* __thiscall HandleManager::FindContainerNoLock(uint64_t ID, int memoryAddress)
{
	return Hook::CallClassMethod<HandleContainer*, HandleManager*, uint64_t, int>(0xA0BBF0, this, ID, memoryAddress);
}

HandleContainer* __thiscall HandleManager::FindContainer(uint64_t ID, void* memoryAddress)
{
	return Hook::CallClassMethod<HandleContainer*, HandleManager*, uint64_t, void*>(0xA0BB40, this, ID, memoryAddress);
}

void __thiscall HandleManager::DestroyHandleContainer(HandleContainer* container)
{
	return Hook::CallClassMethod<void, HandleManager*, HandleContainer*>(0xA094D0, this, container);
}

void __thiscall HandleManager::OnHardRefCountToZero(HandleContainer* container)
{
	return Hook::CallClassMethod<void, HandleManager*, HandleContainer*>(0xA0C160, this, container);
}

void HandleManager::DecSoftRefCount(HandleContainer* container)
{
	container->m_SoftRefCount--;

	if (container->m_SoftRefCount <= 0 && container->m_HardRefCount <= 0)
	{
		if (HandleManager* manager = GetInstance())
		{
			manager->DestroyHandleContainer(container);
		}
	}

	if (container->m_HardRefCount <= 0)
	{
		if (HandleManager* manager = GetInstance())
		{
			manager->OnHardRefCountToZero(container);
		}
	}
}

void HandleManager::DecHardRefCount(HandleContainer* container)
{
	container->m_HardRefCount--;

	if (container->m_SoftRefCount <= 0 && container->m_HardRefCount <= 0)
	{
		if (HandleManager* manager = GetInstance())
		{
			manager->DestroyHandleContainer(container);
		}
	}

	if (container->m_HardRefCount <= 0)
	{
		if (HandleManager* manager = GetInstance())
		{
			manager->OnHardRefCountToZero(container);
		}
	}
}