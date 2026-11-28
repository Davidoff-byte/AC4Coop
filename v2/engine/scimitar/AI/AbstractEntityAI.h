#pragma once

#include "..\Statechart.h"
#include "..\Components\AIComponent.h"
#include "..\Entity.h"

// Per-entity AI object. The game reaches it through the entity's AI component and runs it as a
// statechart; we only ever hold it as an opaque pointer (AIEntityHelper) and read the two service
// arrays by raw offset, so the member list below exists to keep the layout, not to be used by name.
//
// Verified offsets we rely on (see HANDOFF-STATE): the health service array sits at +0x70 with its
// count at +0x76, and the second service array is at +0x5C; the AI-side alive byte is +0x8C and the
// is-alive bit test lives at +0xB8. Changing the order or the types here would move those.

class EntityDataItem : public Object
{
public:
};

class AbstractEntityAI : public Statechart
{
public:
	BaseSmallArray<EntityDataItem*> m_EntityDataItem;
	void* m_AutonomousLogic;
	BaseSmallArray<void*> m_Sensors;
	BaseSmallArray<void*> m_Services;
	Statechart* m_ReactionHandler;
	Statechart* m_PerceptionHandler;
	void* m_FinalizedLogic;
	AIComponent* m_AIComponent;
	Entity* m_Self;
	bool m_IsFinalized;
	BaseSmallArray<void*> m_ComponentContainer;
};
