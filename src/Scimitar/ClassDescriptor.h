#pragma once
#include <Windows.h>
#include <cstdint>
#include "Enums.h"
#include "..\Hook.h"
#include "Memory.h"

class ClassDescriptor;

typedef void (*inplaceConstructorMemberPtr)(void*);
typedef void (*serializeBaseObjectMemberPtr)(void*, void*);
typedef void* (*duplicateBaseObjectMemberPtr)(void*, void*, DuplicateMode);
typedef void (*callBridgeBaseObjectMemberPtr)(void*, void*);
typedef void (*fastLoadBaseObjectMemberPtr)(void*, void*);
typedef void (*staticCallBridgeMemberPtr)(void*);
typedef void (*intializeObjectMemberPtr)(void*, InitType, void*);
typedef void (*destructObjectMemberPtr)(void*);
typedef void (*releaseResourcesObjectMemberPtr)(void*);

class TypeData
{
public:
	uint64_t m_ID : 32;
	uint64_t m_ArraySize : 15;
	uint64_t m_Generic : 1;
	uint64_t m_Type : 6;
	uint64_t m_BitField : 1;
	uint64_t m_AssociatedType_Or_BitSize : 6;
	uint64_t m_BitFieldSize : 2;
	uint64_t m_FillerBits : 1;
};

class PropertyDirectAccessInfoData
{
public:
	unsigned int m_BaseType : 5;
	unsigned int m_BitOffset : 6;
	unsigned int m_BitSize : 5;
	unsigned int m_BitField : 1;
	unsigned int m_Dynamic : 1;
	unsigned int m_Offset : 14;
};

class __declspec(align(8)) PropertyAccessInfo
{
public:
	TypeData m_Type;
	PropertyDirectAccessInfoData m_DirectAccessInfo;
	void* m_GetCallback;
	void* m_SetCallback;
	void* m_AddOrRemoveFromCallback;

	inline int GetTypeID()
	{
		return m_Type.m_Type;
	}
};

class PropertyDescriptor
{
public:
	class PropertyFlags
	{
	public:
		unsigned int pf_Persistent : 1;
		unsigned int pf_InitValue : 1;
		unsigned int pf_ReadCallback : 1;
		unsigned int pf_WriteCallback : 1;
		unsigned int pf_Virtual : 1;
		unsigned int pf_GameStateInit : 1;
		unsigned int pf_PropertyErrorSearchable : 1;
		unsigned int pf_ReadOnly : 1;
		unsigned int pf_WriteOnly : 1;
		unsigned int pf_Owner : 1;
		unsigned int pf_DynamicProperty : 1;
		unsigned int pf_Searchable : 1;
		unsigned int pf_SearchableIndexed : 1;
		unsigned int pf_SearchableKey : 1;
		unsigned int pf_OwnerRef : 1;
		unsigned int pf_DirtyFlag : 1;
		unsigned int pf_DynamicFlag : 1;
		unsigned int pf_Tool : 1;
		unsigned int pf_Resource : 1;
		unsigned int pf_ResourceMember : 1;
		unsigned int pf_AutoSave : 1;
		unsigned int pf_LoadOnDemand : 1;
		unsigned int pf_WeakLink : 1;
		unsigned int pf_ToolSeparate : 1;
		unsigned int pf_FastLoadConditionalSave : 1;
		unsigned int pf_FastLoadPersistent : 1;
		unsigned int pf_ConditionalSave : 1;
		unsigned int pf_SaveGame : 1;
		unsigned int pf_Sorted : 1;
		unsigned int pf_FastLoadHandleRefMustExist : 1;
		unsigned int pf_PartialSerialization : 1;
		unsigned int pf_ExecuteEnginePostArrowStep : 1;
	};

	PropertyFlags m_Flags;
	unsigned int m_PropertyID;
	PropertyAccessInfo m_PropertyAccessInfo;

	inline ClassDescriptor* GetClassDescriptor(void* object, bool ptrIsDirect)
	{
		return Hook::CallClassMethod<ClassDescriptor*, PropertyDescriptor*, void*, bool>(0x00A07980, this, object, ptrIsDirect);
	}
};

class __declspec(align(8)) ClassDescriptor
{
public:
	class ClassFlags
	{
	public:
		unsigned int cf_Separate : 1;
		unsigned int cf_DynamicProperties : 1;
		unsigned int cf_Interface : 1;
		unsigned int cf_Final : 1;
		unsigned int cf_ArrowEvent : 1;
		unsigned int cf_Init : 1;
		unsigned int cf_SkipIndexing : 1;
		unsigned int cf_Loadable : 1;
		unsigned int cf_Embedded : 1;
		unsigned int cf_KeyToDataObject : 1;
		unsigned int cf_BlockAllocator : 1;
		unsigned int cf_Resource : 1;
		unsigned int cf_Tool : 1;
		unsigned int cf_GraphicThread : 1;
		unsigned int cf_MustNotBeControlled : 1;
		unsigned int cf_NoUnsharing : 1;
		unsigned int cf_SaveGame : 1;
		unsigned int cf_NoInit : 1;
		unsigned int cf_EmbedDatabaseRelations : 1;
		unsigned int cf_SerializeInEngine : 1;
		unsigned int cf_KeepReferenceToFatherOnInstantiation : 1;
	};

	PropertyDescriptor* m_Properties;
	void* m_Enums;
	void* m_Calls;
	const char* m_ClassName;
	unsigned int m_ParentID;
	unsigned int m_ClassID;
	int m_ClassSize;
	ClassFlags m_ClassFlags;
	int m_DynamicPropertiesOffset;
	ClassDescriptor* m_ParentClassDescriptor;
	int m_ClassIndex;
	WORD m_RangeInheritanceMin;
	WORD m_RangeInheritanceMax;
	inplaceConstructorMemberPtr m_InplaceConstructor;
	serializeBaseObjectMemberPtr m_Serialize;
	duplicateBaseObjectMemberPtr m_Duplicate;
	callBridgeBaseObjectMemberPtr m_CallBridge;
	fastLoadBaseObjectMemberPtr m_FastLoad;
	staticCallBridgeMemberPtr m_StaticCallBridge;
	intializeObjectMemberPtr m_InitializeObject;
	destructObjectMemberPtr m_DestructObject;
	releaseResourcesObjectMemberPtr m_ReleaseResourcesObject;
	WORD m_NbProperties;
	BYTE m_NbEnums;
	BYTE m_NbCalls;
	BYTE m_ClassAlignment : 7;
	BYTE m_NeedToSkipReleaseResources : 1;

	inline void* CreateObject()
	{
		void* obj = Memory::Allocate(m_ClassSize);
		m_InplaceConstructor(obj);
		return obj;
	}
};