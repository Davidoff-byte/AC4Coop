#pragma once

#include "BaseTypes.h"
#include "Entity.h"
#include "World.h"
#include "Components\Component.h"
#include "..\HookMath.h"

class ContactPoint
{
public:
	enum ContactType
	{
		Ground_0 = 0x1,
		Slope = 0x2,
		Wall = 0x4,
		Ceiling = 0x8,
		Invisible = 0x10,
		Any = 0xFFFFFFFF,
	};

	ContactPoint()
	{
		m_Distance = 0.0;
		m_ProjectedVelocity = 0.0;
		m_Type = ContactType::Any;
		m_ShapeKeyIndexAndId = 0;
		m_MyShapeKeyIndexAndId = 0;
		m_PhysicComponent = GuardedComponentPtr();
	}

	HookMath::Vector4 m_InternalPoint;
	HookMath::Vector4 m_Normal;
	float m_Distance;
	float m_ProjectedVelocity;
	ContactPoint::ContactType m_Type;
	GuardedComponentPtr m_PhysicComponent;
	unsigned int m_ShapeKeyIndexAndId;
	unsigned int m_MyShapeKeyIndexAndId;
};

class CollisionFilterInfo
{
public:
	enum Layer
	{
		LayerNone = 0x0,
		Static = 0x1,
		Dynamic = 0x2,
		Debris = 0x3,
		DetectionZone = 0x4,
		Character_0 = 0x5,
		Ragdoll = 0x6,
		NoCharacterCollision = 0x7,
		Nothing = 0x8,
		Interactive = 0x9,
		CharacterNoInteractive = 0xA,
		OnlyStatic = 0xB,
		Horse = 0xC,
		HorseBarrier = 0xD,
		WaterLevel = 0xE,
		CharacterNoStatic = 0xF,
		MainCharacter = 0x10,
		MainCharacterNoInteractive = 0x11,
		MainCharacterNoStatic = 0x12,
		FuzzyZone = 0x13,
		FuzzyZoneAllCharacter = 0x14,
		HollywoodMode = 0x15,
		StaticNoHorse = 0x16,
		NPCBarrier = 0x17,
		Projectile = 0x18,
		ProjectileBarrier = 0x19,
		HorseNoMainCharacterCollision = 0x1A,
		OnlyDynamic = 0x1B,
		NoHorseCollision = 0x1C,
		BoatCapsule = 0x1D,
		BoatBarrier = 0x1E,
		MainBoatCapsule = 0x1F,
		OnlyStaticAndFuzzy = 0x20,
		ShipHull = 0x21,
		ShipPart = 0x22,
		ShipPartNoCharacter = 0x23,
		NavalMine = 0x24,
		Targetable = 0x25,
		Weapon = 0x26,
		DivableWater = 0x27,
		LOSBreaker = 0x28,
		Disabled = 0x29,
		FloatingObject = 0x2A,
		LayerSize = 0x2B,
		Raycast = 0x2C,
		RaycastOnlyCharacters = 0x2D,
		RaycastNoCharacters = 0x2E,
		Camera = 0x2F,
		StaticNoCamera = 0x30,
		FuzzyZoneNoCamera = 0x31,
		StaticNoLOS = 0x32,
		StaticNoCameraNoLOS = 0x33,
		OnlyLOS = 0x34,
		FuzzyZoneAllCharacterNoCamera = 0x35,
		FuzzyZoneNoCameraAllowAssassination = 0x36,
		OcclusionHeightTest = 0x37,
		OnlyLOSAndFuzzy = 0x38,
		OnlyStaticAndCameraRaycast = 0x39,
		RaycastNoCharactersFuzzyZone = 0x3A,
		OnlyFuzzy = 0x3B,
		RaycastFuzzyZone = 0x3C,
		RagdollPenetrationSolver = 0x3D,
		ProjectileCameraBarrier = 0x3E,
		CameraBarrierPlusLOS = 0x3F,
		CameraBarrier = 0x40,
	};

	enum Part
	{
		PartNone = 0x0,
		TorsoPelvisAndHead = 0x1,
		LeftArmUpper = 0x2,
		LeftArmLower = 0x3,
		RightArmUpper = 0x4,
		RightArmLower = 0x5,
		LeftLegUpper = 0x6,
		LeftLegLower = 0x7,
		RightLegUpper = 0x8,
		RightLegLower = 0x9,
		TailUpper = 0xA,
		TailLower = 0xB,
		PartSize = 0xC,
	};

	enum OwnerType
	{
		PHANTOM = 0x0,
		ENTITY = 0x1,
		COLLIDABLE = 0x2,
		COLLISIONMATERIAL = 0x3,
	};

	class FilterInfo
	{
	public:
		unsigned int m_Layer : 6;
		unsigned int m_Part : 4;
		unsigned int m_Group : 6;
		unsigned int m_Instance : 10;
		unsigned int m_NoCameraCollision : 1;
		unsigned int m_NoCameraOcclusion : 1;
		unsigned int m_NoLineOfSightOcclusion : 1;
		unsigned int m_NoWeatherExposed : 1;
		unsigned int m_NoFreeAimingOcclusion : 1;
		unsigned int m_CustomFilterFlag6 : 1;
	};

	FilterInfo m_FilterInfo;
	CollisionFilterInfo::OwnerType m_OwnerType;
	void* m_OwnerPointer;

	bool SetLayer(int layer);
};

class CollisionQueryCollisionFilter
{
public:
	static uint64_t GetCollisionsEnabledAsMask(CollisionFilterInfo::Layer layer);
};

class RayCast
{
public:
	static bool RayCastHitTest(World* world, void* physicWorkspace, ContactPoint* outContactPoint, Entity* excludedEntity, HookMath::Vector4* rayStart, HookMath::Vector4* rayEnd, uint64_t layerQueryMask, bool collectBackFace, bool useWallNormal, bool useGroundNormal);
	static Entity* RayCastHitEntity(void* physicWorkspace, Entity* excludedEntity, HookMath::Vector4* rayStart, HookMath::Vector4* rayEnd, uint64_t layerQueryMask, bool collectBackFace, bool useWallNormal, bool useGroundNormal);
};
