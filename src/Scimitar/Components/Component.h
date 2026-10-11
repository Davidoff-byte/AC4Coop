#pragma once

#include "..\ManagedObject.h"
#include "..\Entity.h"

class Entity;
class Component;

class GuardedComponentPtr
{
public:
	GuardedComponentPtr()
	{
		m_Component = nullptr;
		m_EntitySequenceNumber = 0;
		m_Entity = HandleContainer::GetInvalidHandle();
	}

	Component* m_Component;
	unsigned int m_EntitySequenceNumber;
	HandleContainer* m_Entity;
};

class Component : public ManagedObject
{
public:
	Entity* m_Owner;

	int SetEnabled(bool enabled);

	inline bool GetEnabled()
	{
		return (m_ObjectFlags & 0x4000000) != 0;
	}
};
