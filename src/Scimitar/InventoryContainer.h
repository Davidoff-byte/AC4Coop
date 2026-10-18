#pragma once

#include "BaseTypes.h"
#include "BoneHandle.h"

// One inventory slot: the bone the item hangs from, a reference that keeps the item resource alive,
// and the handle itself.
class InventorySlot
{
public:
	BoneHandle m_AttachBone;
	Reference m_ItemRef;
	Handle m_Item;
};

// A fixed-size bag. Nothing in this project reads it by member name - Human embeds it by value as
// m_Inventory[38], so the only thing that matters here is that the sizes and order stay exactly as
// the engine lays them out.
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
