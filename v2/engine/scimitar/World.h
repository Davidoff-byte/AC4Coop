#pragma once

#include "BaseTypes.h"
#include "Entity.h"
#include "..\HookMath.h"

class World;
class WorldComponent;
class MandatoryWorldComponent;
class FightManager;
class NavigationManager;
class GameplayCoordinatorWorldComponent;

class GridCellSearchSubResult
{
public:
	int m_CellID;
	int m_IndexXAtLevel;
	int m_IndexYAtLevel;
};

class GridCellSearchResult
{
public:
	int m_AdjustedGridDimensionAtLevel;
	int m_NbrOfCellsToSkip;
	int m_AdjustedCellSizeAtLevel;
	GridCellSearchSubResult m_BottomLeftCorner;
	GridCellSearchSubResult m_TopRightCorner;
	void* m_Grid;
};

// Opaque evaluators; only their footprint matters (16 bytes each, embedded in World).
class WaterLevelFunctor
{
	char padding[16];
};

class WaterLevelOnlyFunctor
{
	char padding[16];
};

class WaterLevelShiftFunctor
{
	char padding[16];
};

class WaterLevelManager_Cache
{
public:
	HookMath::Vector4 m_LocalMin;
	HookMath::Vector4 m_LocalMax;
	HookMath::Vector4 m_CachedWorldMin;
	HookMath::Vector4 m_CachedWorldMax;
	const World* m_World;
	void* m_PhysicWorkspace;
	bool m_HasWater;
	bool m_UseNavMesh;
};

class ComponentList
{
public:
	BaseBigArray<void*> m_Components;
	AdaptiveLock m_Lock;
};

class EntityManager
{
public:
	World* m_Owner;
	BaseBigArray<Reference> m_Entities;
	BYTE m_DebugDisplayFlags;
	ComponentList m_ComponentLists[40];
	BaseSmallArray<void*> m_CullFlagsEntities;
	AdaptiveLock m_Lock;
};

class GridLayout
{
public:
	int m_CellSize;
	int m_BottomLeftX;
	int m_BottomLeftY;
	int m_BottomLeftZ;
	int m_GridDimensionLevel0;
	WORD m_DefaultLoadingDistance;
};

class WorldEngineData
{
public:
	float m_DeathHeight;
	float m_CharacterLODSwitchDistanceMultiplierInverse;
	BYTE m_MaxSoundDistance;
};

class FloorHeightData
{
public:
	enum CompressMode
	{
		INT_16BPP = 0x0,
		DXT_4BPP = 0x1,
	};

	HookMath::Vector4 m_Origin;
	BaseBigArray<WORD> m_HeightSamples;
	BaseBigArray<uint64_t> m_HeightSamplesDXT;
	BaseBigArray<float> m_HeightSamplesf;
	float m_ScalingFactor;
	float m_Offset;
	uint32_t m_TileWidth;
	uint32_t m_TileHeight;
	BaseSmallArray<uint64_t> m_ChunkProxySample;
	uint32_t m_CellSize;
	CompressMode m_CompressMode;
	WORD m_ChunksPerDimension;
	bool m_IsValid;
	bool m_CompressValues;
	bool m_SplitHeightDataOnGrid;
	BaseSmallArray<uint64_t*> m_ChunkHeightSamplesDXTPtr;
	uint32_t m_ChunkTilesWidth;
	BYTE m_ChunkTilesWidthShifter;
};

class SpatialTopology : public ManagedObject
{
public:
	Handle m_AssociatedWorld;
	GridLayout* m_GridLayout;
	void* m_LoadingAdvisor;
};

class GridPartition : public SpatialTopology
{
public:
	int m_NumberOfLevels;
	BaseBigArray<WORD> m_LoadingRangeTable;
	BaseBigArray<void*> m_Cells;
	int m_CellsLoadStatusDataBlock;
	AdaptiveLock m_LoadStatusLock;
	WORD m_NotificationClientID;
	int m_DataSourceID;
	int m_ExtraDataSourceID;
	BYTE m_MainGrid : 1;
	BYTE m_IsLongRangeGrid : 1;
};

class LoadingAdvisor
{
public:
	enum DataLayerChangesStatus
	{
		DataLayersNoChange = 0x0,
		DataLayersUnloadingData = 0x1,
		DataLayersLoadingData = 0x2,
	};

	GridCellSearchResult m_LastCellsSearchResult;
	uint32_t m_NbrLoadPendingLevel0;
	uint32_t m_NbrLoadPendingTotal;
	BYTE m_SlotLayersChanged[28];
	DataLayerChangesStatus m_DataLayersChangesStatus;
	BaseSmallArray<void*> m_SubAdvisors;
	World* m_AssociatedWorld;
	Handle m_PlayerSpaceReference;
	Handle m_SpaceReferenceOverride;
	uint32_t m_LastSearchFrame;
	uint32_t m_LastReferencePositionFrame;
	uint32_t m_CustomLoadingRange;
	uint32_t m_MinimumPossibleLoadingDistance;
	HookMath::Vector4 m_DefaultSearchPos;
	HookMath::Vector4 m_ReferencePosition;
	uint32_t m_WaitTimeBeforeHandlingDataLayers;
	BYTE m_IsLongRangeLoadingAdvisor : 1;
	BYTE m_IsAdvisorActive : 1;
	BYTE m_IsDynamicLoadingActive : 1;
	BYTE m_IsLoadingCompleted : 1;
	BYTE m_IsLoadingCompletedNear : 1;
	BYTE m_IsWorldPausedByLoading : 1;
	BYTE m_ForceInitialPosition : 1;
	BYTE m_HasTeleported : 1;
	BYTE m_IsDynamicLoadingDisabledForever : 1;
	float m_MoveSpeed;
	HookMath::Vector4 m_LastLoadOrientation;
	HookMath::Vector4 m_LastLoadPosition;
	int m_LastLoadPositionCellID;
	float m_MinimalDistanceForReload;
	GridPartition* m_GridPartition;
	bool m_UseLoadingHysteresis;
	LoadingAdvisor* m_SlaveLoadingAdvisor;
	int m_LastLoadingDistance;
};

// A loaded world. Everything below is state the engine owns; we only call into it.
//
// GetCurrentWorld (0x54D4D0) reads a global world manager and returns its +0x50 pointer, or null when no
// world is up - that null is the normal state during loading, so every caller must check it.
// GetPlayer (0x70E810) resolves the player entity through the world's entity manager, not by scanning.
class World : public ManagedObject
{
public:
	float m_FakeActivationDistance;
	Reference m_WorldDataLayerManager;
	Reference m_DefaultTransitionPortal;
	BaseSmallArray<Reference> m_WorldTransitionPortals;
	float Unk00;
	uint64_t m_StartupPlayerSpawner;
	BaseSmallArray<WorldComponent*> m_Components;
	BaseSmallArray<Reference> m_WorldAreas;
	Reference m_PropagationMap;
	Handle m_StartupTransitionPortal;
	Reference m_LongRangeGrid;
	Reference m_FakeEntities;
	Handle m_FakeEntitiesHandle;
	Reference m_Grid;
	BaseSmallArray<uint64_t> m_ObjectsNotLoadedInSubWorld;
	void* m_WorldDataLayerStates;
	void* m_WorldGraphicData;
	void* m_FXManager;
	void* m_ContactManager;
	void* m_RegionLayoutManager;
	BaseSmallArray<Handle> m_Zombies;
	float m_UserCullingPlanesNear;
	float m_UserCullingPlanesFar;
	EntityManager m_EntityManager;
	GridLayout m_GridLayout;
	GridLayout m_LongRangeGridLayout;
	WorldEngineData m_WorldEngineData;
	FloorHeightData m_FloorHeightData;
	bool m_IsFakeEntitiesInLongRangeGrid;
	bool m_IsLongRangeGridActive;
	bool m_EnableUserCullingPlanes;
	BYTE m_UserCullingPlanesDescriptorMask;
	WaterLevelFunctor m_FindWaterLevelEvaluator;
	WaterLevelOnlyFunctor m_FindWaterLevelOnlyEvaluator;
	WaterLevelShiftFunctor m_FindWaterLevelShiftEvaluator;
	BaseSmallArray<Entity*> m_CharacterList;
	BaseSmallArray<Entity*> m_LookAtList;
	BaseSmallArray<Entity*> m_AttractorList;
	BaseSmallArray<Entity*> m_ObjectList;
	void* m_PhysicWorld;
	void* m_AIEngine;
	void* m_GraphicWorld;
	int m_DataSourceID;
	int m_ExtraDataSourceID;
	LoadingAdvisor* m_LoadingAdvisor;
	LoadingAdvisor* m_LongRangeLoadingAdvisor;
	void* m_ActivationEventListener;
	void* m_WorldTasks;
	void* m_pVisualOceanComponentCache;
	Reference m_FakeEntitiesAlwaysAlive;

	inline static World* GetCurrentWorld()
	{
		return Hook::CallCdeclMethod<World*>(0x0054D4D0);
	}

	static void LoadWorldAsync(uint64_t worldID, bool pauseAfterLoad, int callback, int numberOfFramesToDelay, uint64_t portalID, bool merge);
	static char* GetWorldAlternateSourcePrefixName(uint64_t worldID);

	void AddEntity(Entity* entity);
	void RemoveEntity(Entity* entity);
	WorldComponent* GetWorldComponentByType(ClassDescriptor* classDescriptor);
	HandleContainer* GetHandle();
	Entity* GetPlayer();
	void SetSlowdownFactor(float factor);
	void Pause(bool pause);
	void PauseWorld(bool forMenu);
	void UnpauseWorld();
};
