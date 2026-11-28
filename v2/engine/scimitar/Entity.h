#pragma once

#include "..\HookMath.h"
#include "GraphicTypes.h"
#include "Hierarchy.h"
#include "Components/Component.h"

class Component;

// Part of an entity that exists without an identity of its own: hierarchy link, a few editor flags and the
// world matrix everything else is derived from. Position and rotation are read straight out of row 4 of that
// matrix, which is the path the live mod uses for every position/teleport.
class BaseEntity : public ManagedObject
{
public:
	class CustomFlags
	{
	public:
		uint32_t m_RenderGizmo : 1;
		uint32_t m_RenderSkeleton : 1;
		uint32_t m_OcclusionEnabled : 1;
	};

	Hierarchy* m_Hierarchy;
	CustomFlags m_CustomFlags;
	HookMath::Transform m_GlobalMatrix;	// translation in M41..M43

	BaseEntity* GetFatherEntity();

	inline void GetRotation(HookMath::Quaternion* q)
	{
		HookMath::FromMatrix(&m_GlobalMatrix, q);
	}

	inline HookMath::Vector4 GetPosition()
	{
		HookMath::Vector4 pos;
		pos.x = m_GlobalMatrix.M41;
		pos.y = m_GlobalMatrix.M42;
		pos.z = m_GlobalMatrix.M43;
		pos.w = 0.0f;
		return pos;
	}

	inline void SetPosition(HookMath::Vector4 pos)
	{
		m_GlobalMatrix.M41 = pos.x;
		m_GlobalMatrix.M42 = pos.y;
		m_GlobalMatrix.M43 = pos.z;
	}

	int SetGlobalRotation(HookMath::Quaternion* rot);

	char SetSight(HookMath::Vector4* sight, HookMath::Vector4* banking);
};

class EntityDescriptor
{
public:
	int m_TypeFlags;
};

// A world entity: identity comes from the handle container, everything here is state.
//
// The two things our code depends on are the global matrix (see BaseEntity) and the component caches at the
// end - components are looked up through GetComponentByType rather than by walking m_Components, because a
// duplicate/ragdoll entity can have stale entries in the array.
class Entity : public BaseEntity
{
public:
	class Flags
	{
	public:
		uint64_t m_SkipGroupMatrixUpdate : 1;
		uint64_t m_UseManualPhysicMatrixUpdate : 1;
		uint64_t m_IsSpawned : 1;
		uint64_t m_IsVisible : 1;
		uint64_t m_WasVisible : 1;
		uint64_t m_IsPhantom : 1;
		uint64_t m_IsStatic : 1;
		uint64_t m_IsHidden : 1;
		uint64_t m_IsSmallObject : 1;
		uint64_t m_IsMediumObject : 1;
		uint64_t m_RenderLast : 2;
		uint64_t m_ForceIncludedInReflection_CurrentGen : 1;
		uint64_t m_ForceExcludedFromReflection_CurrentGen : 1;
		uint64_t m_EagleVisionIsMainTarget : 1;
		uint64_t m_EagleVisionCanBeAssassinated : 1;
		uint64_t m_WasHidden : 1;
		uint64_t m_HasMovedFromOrigin : 1;
		uint64_t m_WasHiddenByAFakeEntity : 1;
		uint64_t m_IsInitialized : 1;
		uint64_t m_CurrentLODLevel : 3;
		uint64_t m_LODLevelDirty : 1;
		uint64_t m_IsCharacter : 1;
		uint64_t m_ShadowCaster_CurrentGen : 1;
		uint64_t m_ShadowReceiver : 1;
		uint64_t m_SunlightReceiver : 1;
		uint64_t m_DynamicLightsReceiver : 1;
		uint64_t m_ForbidNotVisibleLODFromRagdoll : 1;
		uint64_t m_HasSkeletonComponent : 1;
		uint64_t m_HasConstraintComponent : 1;
		uint64_t m_UseVisibilityQueries : 1;
		uint64_t m_VisibilityOccluder : 1;
		uint64_t m_AlwaysVisibilityOccluder : 1;
		uint64_t m_IgnoreOcclusionForLODLevel : 1;
		uint64_t m_CanUsePreviousOcclusionForLODLevel : 1;
		uint64_t m_CanUseOcclusionForLODLevel : 1;
		uint64_t m_RequiresCullingInfo : 1;
		uint64_t m_IgnoreUserCullingPlanes : 1;
		uint64_t m_IsHiddenByUserCullingPlanes : 1;
		uint64_t m_UseExactSkeletonUpdateThreadPath : 1;
		uint64_t m_UseSynchedDisplacementThreadPath : 1;
		uint64_t m_UseAIInteractionMultiThreadPath : 1;
		uint64_t m_LockLODToNearest : 1;
		uint64_t m_LockLODToFarthest : 1;
		uint64_t m_EntityCategory : 5;
		uint64_t m_UserMask1 : 1;
		uint64_t m_UserMask2 : 1;
		uint64_t m_UserMask3 : 1;
		uint64_t m_UserMask4 : 1;
		uint64_t m_UserMask5 : 1;
		uint64_t m_OnEngineStartFlag : 1;
		uint64_t m_WeatherExposed : 1;
	};

	Entity::Flags m_EntityFlags;
	void* m_DataOwner;
	void* m_AssociatedWorld;
	BaseSmallArray<Component*> m_Components;
	Reference Unk00;
	int m_FakeCellIndex;
	float m_Scale;
	float m_CameraSquareDistance;
	GameStateData* m_ResetData;
	BoundingVolume m_BoundingVolume;
	EntityDescriptor m_EntityDescriptor;
	DataLayerFilter m_DataLayerFilter;
	WORD m_NotificationClientID;
	void* m_UnshareContext;
	GraphicNode* m_GraphicNode;
	GraphicNodeUpdateTag m_GraphicNodeUpdateTag;
	int m_GraphicWorldIndex;
	void* m_PrimalAssociatedWorld;
	int m_EntityManagerIndex;
	Handle m_HandleCache;
	int m_EntitySequenceNumber;
	int m_WorldOperationsCount;
	void* m_EntityAiLock;
	ComponentCacheArray m_VisualComponentsCache;
	ComponentCacheArray m_PhysicsComponentsCache;
	ComponentCacheArray m_GuidanceComponentsCache;
	ComponentCache m_SkeletonComponentCache;
	ComponentCache m_BehaviorComponentCache;
	void* m_AIComponent;
	ComponentCache m_EventListenerComponentCache;
	ComponentCache m_SoundComponentCache;
	BYTE m_ComponentsVersion;
	bool m_IsOccludedInOpaque;
	bool m_IsOccludedInReflection;

public:
	int Duplicate();
	void SetIsHidden(int hidden);
	Component* GetComponentByType(ClassDescriptor* componentDescriptor);
	BaseEntity* GetHighestParentEntity();
	void* GetBehaviorComponent();
	void FlagsChanged();
	void ShadowFlagsChanged();
	void ReflectionFlagsChanged();
	void SetGlobalMatrixInternal(HookMath::Transform* matrix, bool updatePhysics);
	void OnAddToWorld(void* world);

	inline bool GetIsHidden() { return m_EntityFlags.m_IsHidden; }
};
