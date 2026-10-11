#pragma once

#include "..\Statechart.h"
#include "..\Components\AIComponent.h"
#include "..\Entity.h"

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