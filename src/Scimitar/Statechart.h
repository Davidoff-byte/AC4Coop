#pragma once

#include "BaseTypes.h"

class Statechart;

class StatechartState : public Object
{
public:
	void* m_StateDesc;
	StatechartState* m_Parent;
	Statechart* m_Statechart;
};

class Statechart : public Object
{
public:
	BaseSmallArray<StatechartState*> m_ActiveStates;
	void* m_StatechartDesc;
	BaseSmallArray<void*> m_History;
	BaseSmallArray<uint64_t> m_DynamicStateInheritanceMinMax;
	Statechart* m_RootStatechart;
	StatechartState* m_EmbeddedParentState;
	Statechart* m_EmbeddedParentStatechart;
	void* m_EmbeddedParentStateDesc;
	BaseSmallArray<Statechart*> m_EmbeddedStatecharts;
	WORD m_EmbeddedDepth;
	BYTE m_EmbeddedParentStateIndex;
	int m_Processing;
	SharedBitArray m_InternalActiveStatesBitArray;
	void* m_StatechartRootData;
	void* m_CurrentEnterStateDesc;
	BYTE* m_StateMemoryBuffer;
	WORD m_SnifferListenerStamp;

	void HandleTrigger(int triggerID, void* statecharCache);
};