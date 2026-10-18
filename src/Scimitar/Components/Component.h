#pragma once

#include "..\ManagedObject.h"
#include "..\Entity.h"

class Entity;
class Component;

// A component pointer that stays safe across entity recycling: the sequence number and the owning
// entity's handle let the engine tell a live pointer from one left over by a respawned entity. We read
// only m_Component (raycast contact points); the two guards are carried to keep the layout.
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

// Base of every entity component. The enabled bit lives in the managed-object flags dword: the engine's
// SetEnabled (0x7130C0) sets or clears 0x4000000 with an interlocked exchange on this+4, which is what
// GetEnabled tests.
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
