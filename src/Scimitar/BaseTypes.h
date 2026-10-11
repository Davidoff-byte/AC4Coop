#pragma once

#include <Windows.h>
#include <cstdint>
#include "ManagedObject.h"
#include "Memory.h"
#include "..\HookMath.h"

class ObjectAllocator : public ManagedObject
{
public:
	void* m_ObjectBlockAllocatorData;
};

#pragma pack(push, 1)
class HandleContainer
{
public:
	ManagedObject* m_Object;
	int m_SoftRefCount;
	int m_HardRefCount;

	inline static HandleContainer* GetInvalidHandle()
	{
		return *(HandleContainer**)0x027F3F90;
	}

	uint64_t GetObjectID() {
		return m_ObjectID;
	}

	uint64_t m_ObjectID;
};
#pragma pack(pop)
class Handle
{
public:
	HandleContainer* m_HandleContainer;

	inline static Handle* New()
	{
		return (Handle*)Memory::Allocate(4, 4);
	}

	inline void* DeleteObject()
	{
		if (m_HandleContainer)
		{
			if (m_HandleContainer->m_Object)
			{
				m_HandleContainer->m_Object->Delete();
				m_HandleContainer->m_Object = NULL;
			}

			return NULL;
		}

		return NULL;
	}

	inline bool IsValid()
	{
		if (!m_HandleContainer)
		{
			return false;
		}

		return m_HandleContainer != HandleContainer::GetInvalidHandle();
	}
};

class Reference
{
public:
	HandleContainer* m_HandleContainer;

	inline static Reference* New()
	{
		return (Reference*)Memory::Allocate(4, 4);
	}

	inline void* DeleteObject()
	{
		if (m_HandleContainer)
		{
			if (m_HandleContainer->m_Object)
			{
				m_HandleContainer->m_Object->Delete();
				m_HandleContainer->m_Object = NULL;
			}

			return NULL;
		}

		return NULL;
	}

	inline bool IsValid()
	{
		if (!m_HandleContainer)
		{
			return false;
		}

		return m_HandleContainer != HandleContainer::GetInvalidHandle();
	}
};

class SmallArrayData
{
public:
	void* m_ArrayData;
	int m_SizeUnion;

	inline int GetSize()
	{
		return m_SizeUnion & 0xFFFF;
	}

	inline int GetIsNotMemoryOwner()
	{
		return (m_SizeUnion >> 31) & 0x1;
	}

	inline int GetIsNotObjectOwner()
	{
		return (m_SizeUnion >> 30) & 0x1;
	}

	inline int GetReservedSize()
	{
		return (m_SizeUnion >> 16) & 0x3FFF;
	}

	inline void SetData(void* newData)
	{
		m_ArrayData = newData;
	}

	inline void SetSize(WORD size)
	{
		m_SizeUnion |= size;
		m_SizeUnion |= ((size & 0x3FFF) << 16);
	}
};

class BigArrayData
{
public:
	void* m_ArrayData;
	int m_Size;
	int m_Flags;

	inline int GetSize()
	{
		return m_Size;
	}

	inline void SetSize(int size)
	{
		m_Size = size;
		m_Flags = 0xC0000000 | size;
	}

	inline void SetData(void* newData)
	{
		m_ArrayData = newData;
	}
};

template <typename T> class BaseSmallArray
{
private:
	SmallArrayData m_Data;

public:
	inline T* GetElement(int index)
	{
		if (index >= m_Data.GetSize())
		{
			return NULL;
		}

		return ((T*)m_Data.m_ArrayData + index);
	}

	inline int GetSize()
	{
		return m_Data.GetReservedSize();
	}

	inline void SetSize(int size)
	{
		m_Data.SetSize(size);
	}

	inline void SetData(void* newData)
	{
		m_Data.SetData(newData);
	}
};

template <typename T> class BaseBigArray
{
private:
	BigArrayData m_Data;

public:
	inline T* GetElement(int index)
	{
		if (index >= m_Data.GetSize())
		{
			return NULL;
		}

		return ((T*)m_Data.m_ArrayData + index);
	}

	inline int GetSize()
	{
		return m_Data.GetSize();
	}

	inline void SetSize(int size)
	{
		m_Data.SetSize(size);
	}

	inline void SetData(void* newData)
	{
		m_Data.SetData(newData);
	}

	inline void* GetData()
	{
		return m_Data.m_ArrayData;
	}
};

class SharedBitArray
{
public:
	BaseSmallArray<BYTE> m_Data;
	int m_Size;
};

class ComponentCache
{
public:
	BYTE m_Version;
	BYTE m_Index;
};

class ComponentCacheArray
{
public:
	BYTE m_Version;
	BYTE m_BeginIndex;
	BYTE m_EndIndex;
};


class GameStateData : Object
{
public:
	int m_PropertyCount;
	BaseSmallArray<BYTE> m_DescBuffer;
	BaseSmallArray<BYTE> m_DataBuffer;
};

class DataLayerFilter
{
public:
	SmallArrayData m_Data;
};

class CompiledResource : public Object
{
public:
};

class EventListener : public ManagedObject
{
public:
	BaseSmallArray<void*> m_HandlerTable;
};

class EventHandler
{
public:
	void* vtable;
	void* m_Target;
};

class EventCallback : public EventHandler
{
public:
	void* m_Callback;
	int m_Delta;
};

class AdaptiveLock
{
public:

	int* _impl;
	BYTE m_Shadow[24];
};

class Mask
{
public:
	BYTE m_Value;
};

class Mask16
{
public:
	WORD m_Mask01 : 1;
	WORD m_Mask02 : 1;
	WORD m_Mask03 : 1;
	WORD m_Mask04 : 1;
	WORD m_Mask05 : 1;
	WORD m_Mask06 : 1;
	WORD m_Mask07 : 1;
	WORD m_Mask08 : 1;
	WORD m_Mask09 : 1;
	WORD m_Mask10 : 1;
	WORD m_Mask11 : 1;
	WORD m_Mask12 : 1;
	WORD m_Mask13 : 1;
	WORD m_Mask14 : 1;
	WORD m_Mask15 : 1;
	WORD m_Mask16 : 1;
};

class RefCounted
{
public:
	void* vtable;
	int m_RefCount;
};

class Timer
{
public:
	unsigned long long m_StartTime;
	unsigned long long m_EndTime;
	unsigned long long m_PauseTime;
	bool m_Started;
	void* m_Clock;
};

class Rect
{
public:
	float m_X;
	float m_Y;
	float m_Width;
	float m_Height;
};

class VectorSpringDamper
{
public:
	HookMath::Vector4 m_pos;
	HookMath::Vector4 m_vel;
};

class VectorDoubleSpringDamper
{
public:
	VectorSpringDamper m_IntermediateSpringDamper;
	VectorSpringDamper m_FinalSpringDamper;
};

class FloatDoubleSpringDamper
{
public:
	HookMath::Vector4 m_Data;
};

class SmoothedGroundPlane
{
public:
	VectorDoubleSpringDamper m_NormalDamper;
	FloatDoubleSpringDamper m_VerticalReferencePositionDamper;
	HookMath::Vector4 m_ReferencePoint;
};

class WindForce
{
public:

	float m_Scale;
	bool m_Active;
	bool m_EntityWind;
	bool m_EntityWindExclusive;
	Handle m_Owner;
	ComponentCache m_WindComponentCache;
};

class CollisionReport
{
public:
	BaseSmallArray<void> m_ContactPoints;
};

class GapInfo
{
public:
	HookMath::Vector4 m_HolePosition;
	HookMath::Vector4 m_EdgePosition;
	HookMath::Vector4 m_EdgeNormal;
	Handle m_Entity;
	float m_Depth;
	float m_Distance;
	GuidanceObjectSubType m_EdgeSubType;
	bool m_NoEdge;
	bool m_SlopeDetected;
	HookMath::Vector4 m_LastRayEndPosition;
	bool m_LastRayHitGround;
};

class JumpActionSequence
{
	float m_TimeScale;
	BaseSmallArray<void*> m_Items;
	FreeStepEndPose m_EndPose;
};

class GameplaySurfaceNavType
{
	BYTE m_GameplaySurfaceNavType_Difficult : 1;
	BYTE m_GameplaySurfaceNavType_CrowdSpawn : 1;
	BYTE m_GameplaySurfaceNavType_NoNPCHorse : 1;
	BYTE m_GameplaySurfaceNavType_NoDynamicSpawn : 1;
	BYTE m_GameplaySurfaceNavType_FreeRunStarter : 1;
};