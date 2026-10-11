#pragma once

#include "BaseTypes.h"
#include "BoneHandle.h"

class InventorySlot
{
public:
	BoneHandle m_AttachBone;
	Reference m_ItemRef;
	Handle m_Item;
};

class InventoryContainer
{
public:
	enum AccessType
	{
		AccessType_Stack = 0x0,
		AccessType_First = 0x1,
		AccessType_Virtual = 0x2,
	};

	BaseSmallArray<InventorySlot> m_Slots;
	int m_Spaces;
	int m_Used;
	AccessType m_AccessType;
};