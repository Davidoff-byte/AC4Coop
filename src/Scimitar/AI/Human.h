#pragma once

#include "AIActor.h"
#include "..\BoneHandle.h"
#include "..\InventoryContainer.h"
#include "..\CollisionFilterInfo.h"
#include "..\Components\ActionComponent.h"

class Human;
class HumanData;

class DebugContext : public ActorContext
{
public:
	HumanData* m_GlobalData;
	void* m_LocalData;
	void* IHumanDebug;
	void* IHumanTeleport;
	void* m_FSM;

	int Enter();
	int Exit();
	void Update(int track);
};

class ContactMaterialManager
{
public:
	enum ContactForce
	{
		ContactForce_Null = 0x0,
		ContactForce_Mild = 0x1,
		ContactForce_Medium = 0x2,
		ContactForce_Strong = 0x3,
	};

	class ContactData
	{
		ContactPoint m_ContactPoint;
		ContactForce m_ContactForce;
		bool m_IsSet;
		unsigned long long m_TimeStamp;
	};

	enum Context
	{
		Context_Ground = 0x0,
		Context_Climb = 0x1,
	};

	Entity* m_Entity;
	Context m_Context;
	ContactData m_ContactData[4];
};

class HumanBody
{
public:
	unsigned int m_LockedBodyParts;
	Entity* m_Entity;
	bool m_IKSolverModified;
};

class EdgeClampingSettings
{
public:
	HookMath::Vector4 m_DirectionForRejectAngle;
	float m_EdgeDistance;
	float m_RejectAngle;
};

class ClimbingTargetPatternDetection
{
public:
	Human* m_AIActor;
};

class ClimbingTargetManager
{
public:
	ClimbingTargetPatternDetection m_ClimbingTargetPatternDetection;
	Human* m_AIActor;
};

class IKClimbControler2
{
public:
	class Mixer
	{
	public:
		BaseSmallArray<void*> m_ActiveTracks;
		BaseSmallArray<void*> m_FadeOutTracks;
		float m_MasterBlendRatio;
		float m_LastEvaluatedSequenceTime;
	};

	class Flags
	{
	public:
		__int8 m_UseOverHangCorrection : 1;
		__int8 m_UsePenetratingHeadCorrection : 1;
		__int8 m_UsePenetratingHeadCorrectionDepth : 1;
		__int8 m_UseHangFreePenetrationCorrection : 1;
		__int8 m_UseLedgeTopCrouchCorrection : 1;
	};

	Mixer m_Mixer[4];
	Human* m_Human;
	Entity* m_Self;
	ActionComponent* m_ActCpnt;
	HookMath::Vector4 m_PelvisOffset;
	Flags m_Flags;
	float m_CurrentHeadPenetrationCorrectionHeight;
	float m_CurrentHeadPenetrationCorrectionDepth;
	float m_CurrentHangFreePenetrationCorrectionDepth;
	float m_CurrentHangFreePenetrationSpineSpineRotationRatio;
	float m_CurrenLedgeTopHeadOverShoot;
};

class HumanBodyLock
{
public:
	int m_LockedBodyParts;
	HumanBody* m_HumanBody;
};

class IKTargetController
{
public:
	Entity* m_Entity;
	HumanBodyLock m_HumanBodyLock;
	int m_UsedBodyParts;
	BaseSmallArray<void*> m_IKEffectorControllers;
};

class BodyBanking
{
public:
	float m_CurWholeRotation;
	float m_DestWholeRotation;
	float m_MaxWholeRotation;
	float m_MaxPelvisXOffset;
	float m_MaxPelvisZOffset;
	float m_RotationSpeed;
	BoneHandle m_BoneArray[5];
	BoneHandle m_LeftArm;
	BoneHandle m_RightArm;
	BoneHandle m_Hips;
	Entity* m_Self;
};

class GuidanceReport
{
public:
	enum GuidanceReportFilter
	{
		GuidanceReportFilter_Dynamic = 0x1,
		GuidanceReportFilter_Static = 0x2,
		GuidanceReportFilter_All = 0x3,
	};

	BaseSmallArray<void*> m_ContactsObjects;
	GuidanceReportFilter m_Filter;
};

class ClimbingGuidanceData
{
public:
	char padding[50];
};

class BeamDetector
{
public:
	void* m_PhysicWorkspace;
	void* m_HumanGuidance;
	GuidanceReport m_MasterBeamReport;
	CollisionFilterInfo::Layer m_Layer;
	unsigned int m_LastUpdatedFrame;
};

class GuidanceHoleDetector
{
public:
	float m_CapsuleRadius;
	float m_CapsuleOffsetFront;
	float m_CapsuleOffsetDown;
	BaseSmallArray<void*> m_CutPoint;
};

class HumanGroundCarryObjectData : public Object
{
public:
	enum eCarryObjectEntryType
	{
		eCarryObjectEntryType_Object_Pickup = 0x0,
		eCarryObjectEntryType_Object_Attached_From_Unbalance = 0x1,
		eCarryObjectEntryType_Object_Attached = 0x2,
		eCarryObjectEntryType_MAX = 0x3,
	};

	eCarryObjectEntryType m_CarryObjectEntryType;
	Handle m_CarryObjectCarriedEntity;
	ePickableType m_PickableType;
};

class HumanGroundHurtFightData
{
public:
	WeaponType m_AttackerWeapon;
	HurtDirection m_HurtDirection;
	HurtFightDirection m_HitDirection;
	HookMath::Vector4 m_FacingDirection;
	Handle m_Attacker;
	bool m_IsForceKnockDown;
	bool m_IsProjectile;
	IncapacitateRating m_IncapacitateRating;
	HitLocation m_HitLocation;
};

class HumanGroundDodgeData
{
public:
	DodgeType m_DodgeType;
	Handle m_Collider;
	bool m_ActivateEdgeClamping;
};

class PinDownSlot : public Object
{
public:
	int m_Unk00;
	HookMath::Vector4 m_Position;
	HookMath::Vector4 m_Direction;
	PinDownSlotType m_PinDownSlotType;
};

class HumanGroundPinDownTrapperData
{
public:
	Handle m_Victim;
	PinDownSlot m_PinDownSlot;
};

class HumanGroundPinDownVictimData
{
public:
	Handle m_Trapper;
	PinDownSlot m_PinDownSlot;
};

class HumanGroundBlockData
{
public:
	Handle m_BlockedTarget;
};

class HumanGroundData : public ActorContextData
{
public:
	enum HumanGroundSubState
	{
		SubState_Movement = 0x0,
		SubState_Fight = 0x1,
		SubState_FreeRun = 0x2,
		SubState_OrientedMove = 0x3,
		SubState_Hurt = 0x4,
		SubState_ObstacleCollision = 0x5,
		SubState_Stealth = 0x6,
	};

	enum ObstacleLeanType
	{
		ObstacleLeanType_Hands = 0x0,
		ObstacleLeanType_Feet = 0x1,
	};

	enum CombatStyle
	{
		CombatStyle_Normal = 0x0,
		CombatStyle_Assassin = 0x1,
		CombatStyle_MAX = 0x2,
	};

	enum HumanGroundStage
	{
		HumanGroundStage_Invalid = 0x0,
		HumanGroundStage_Navigation = 0x1,
		HumanGroundStage_Hurt = 0x2,
		HumanGroundStage_Unbalance = 0x3,
		HumanGroundStage_Fight = 0x4,
		HumanGroundStage_PinDown = 0x5,
		HumanGroundStage_Escort = 0x6,
		HumanGroundStage_Assassinate = 0x7,
		HumanGroundStage_AssassinationPose = 0x8,
		HumanGroundStage_GetAssassinated = 0x9,
		HumanGroundStage_BlockActor = 0xA,
		HumanGroundStage_Dodge = 0xB,
		HumanGroundStage_RangeCombat = 0xC,
		HumanGroundStage_GetUp = 0xD,
		HumanGroundStage_PreGrabbed = 0xE,
		HumanGroundStage_Grabbed = 0xF,
		HumanGroundStage_GrabbedBreak = 0x10,
		HumanGroundStage_FleeFight = 0x11,
		HumanGroundStage_CarryObject = 0x12,
		HumanGroundStage_GetTackled = 0x13,
		HumanGroundStage_Tackle = 0x14,
		HumanGroundStage_GrabbedFromBehind = 0x15,
		HumanGroundStage_PickUpWeapon = 0x16,
		HumanGroundStage_ThrowSmokeBomb = 0x17,
		HumanGroundStage_ChainBladeCharge = 0x18,
		HumanGroundStage_ChainBladeAim = 0x19,
		HumanGroundStage_ThrowChainBlade = 0x1A,
		HumanGroundStage_AppleAttack = 0x1B,
		HumanGroundStage_CounterGrab = 0x1C,
		HumanGroundStage_ThrownWeaponHurtLong = 0x1D,
		HumanGroundStage_ThrownWeaponHurtBlunt = 0x1E,
		HumanGroundStage_GrabbedFromBehindEscape = 0x1F,
		HumanGroundStage_ReflexShot = 0x20,
		HumanGroundStage_ThrownWeaponHurtSmall = 0x21,
	};

	HookMath::Vector4 m_CollideNormal;
	HookMath::Vector4 m_DestHeading;
	HookMath::Vector4 m_DestGlobalBodyOrientation;
	HookMath::Vector4 m_LCOActorSight;
	HookMath::Vector4 m_LCOPelvisSight;
	HookMath::Vector4 m_LCOFootSight;
	HookMath::Vector4 m_BlockCylinderOffset;
	HookMath::Vector4 m_CollidePosition;
	HumanGroundSubState m_SubState;
	float m_CurSpeedRatio;
	float m_DestSpeedRatio;
	float m_HeadingLinearRotationSpeed;
	float m_HeadingEasedOutRotationSpeedRatio;
	unsigned int m_ParamFlags;
	unsigned int m_InternalFlags;
	Handle m_EntityToAvoid;
	HumanGroundCarryObjectData m_CarryObjectData;
	float m_CurrentBodyAngle;
	float m_CurrentBodyBankingAngle;
	ObstacleLeanType m_ObstacleLeanType;
	float m_CollideHeight;
	Handle m_CollideEntity;
	float m_BlockCylinderRadius;
	float m_BlockCylinderLength;
	int m_CustomActionControlChannel;
	int m_CustomActionSecondaryChannel;
	Handle m_IdleBreakerActionBlock;
	CombatStyle m_CombatStyle;
	bool m_IsSet;
	bool m_Sprint;
	bool m_BreakHardOnEdge;
	bool m_ForceFallFromGrabThrow;
	bool m_ForceRemainStandingOnAnyWallImpact;
	bool m_BreakHard;
	bool m_ActingPlaying;
	bool m_ActingInterrupted;
	HumanGroundStage m_HumanGroundStage;
	BYTE m_NbMvtTypes;
	BYTE m_NbMvtDivisions[2];
	float m_MvtDivThreshold[2][5];
	BYTE m_CurMvtType;
	BYTE m_DestMvtType;
	BYTE m_CurMvtDivision;
	HookMath::Vector4 m_JumpDirection;
	bool m_IsInSearchState;
	bool m_IsInSearchAround;
	bool m_IsInSearchFail;
	bool m_IsInBlockingAction;
	int m_Unk01;
	Timer m_BlockFallOffEdgeTimer;
	HumanGroundHurtFightData m_HumanGroundHurtFightData;
	HumanGroundDodgeData m_HumanGroundDodgeData;
	HumanGroundPinDownTrapperData m_HumanGroundPinDownTrapperData;
	HumanGroundPinDownVictimData m_HumanGroundPinDownVictimData;
	HumanGroundBlockData m_HumanGroundBlockData;
	Entity* m_FallbackEntityHierarchy;
	bool m_UseHurtFall;
};

class HumanGroundPresentData : public ActorContextData
{
public:
	HookMath::Vector4 m_DestHeading;
	float m_CurSpeedRatio;
	float m_DestSpeedRatio;
	bool m_BreakHard;
	bool m_BreakHardOnEdge;
	bool m_Sprint;
	BYTE m_CurMvtType;
	BYTE m_DestMvtType;
	Entity* m_FallbackEntityHierarchy;
};

class HumanInAirData : public ActorContextData
{
public:
	enum FallOrigin
	{
		FallOrigin_Ground = 0x0,
		FallOrigin_Climb = 0x1,
		FallOrigin_HangWall = 0x2,
		FallOrigin_HangFree = 0x3,
	};

	HookMath::Transform m_JumpInitialOrigin;
	HookMath::Transform m_JumpInitialDestination;
	HookMath::Vector4 m_JumpApexPosition;
	HookMath::Transform m_JumpDisplacementRefMatrix;
	HookMath::Vector4 m_HierarchicalVelocity;
	HookMath::Vector4 m_ReboundDirection;
	HookMath::Vector4 m_LastHangHandsPosition;
	unsigned int m_JumpActionPartB;
	unsigned int m_JumpFallAction;
	unsigned int m_AlternateJumpFallAction;
	BaseSmallArray<float> m_ReceptionBlendFactors;
	BaseSmallArray<float> m_AlternateReceptionBlendFactors;
	FallOrigin m_FallOrigin;
	bool m_IgnoreCharacterCollision;
	bool m_JumpApexReached;
	bool m_UseJumpActionSequence;
	bool m_DeepDestFootHoldIsSet;
	bool m_OrientNormalToLOS;
	JumpActionSequence m_JumpActionSequence;
	JumpType m_JumpType;
	InAirSubState m_SubState;
	bool m_ReboundRequested;
};

class HumanLadderData : public ActorContextData
{
public:
};

class HumanLedgeData : public ActorContextData
{
public:
	HookMath::Vector4 m_CurrentDirection;
	float m_CurrentSpeedRatio;
	uint32_t m_DirectionType;
	float m_SpeedRatio;
	BYTE m_DestMvtType;
};

class HumanClimbingData : public ActorContextData
{
public:
	enum EntryPoseType
	{
		EntryPoseType_Closed = 0x0,
		EntryPoseType_Opened = 0x1,
	};
	
	EntryPoseType m_EntryPoseType;
	HookMath::Vector2 m_CurrentDirection;
	float m_CurrentSpeedRatio;
	Handle m_WaterWell;
	bool m_IsWaterWellLedge;
};

class HumanWallingData : public ActorContextData
{
public:
};

class HumanNarrowObjectData : public ActorContextData
{
public:
};

class HumanSwimData : public ActorContextData
{
public:
};

class HumanDataBundle : public Object
{
public:
	DebugContextData m_DebugData;
	HumanGroundData m_GroundData;
	HumanGroundPresentData m_GroundPresentData;
	HumanInAirData m_HumanInAirData;
	HumanLadderData m_LadderData;
	HumanLedgeData m_LedgeData;
	HumanClimbingData m_ClimbingData;
	HumanWallingData m_WallingData;
	HumanNarrowObjectData m_NarrowObjectData;
	HumanSwimData m_SwimData;
};

class HumanContextBundle
{
public:
	void* m_Ground;
	void* m_GroundPresent;
	void* m_Ladder;
	void* m_InAir;
	void* m_Ledge;
	void* m_Walling;
	void* m_Boat;
	void* m_Rope;
	void* m_Kiosk;
	void* m_Dead;
	void* m_HayStack;
	void* m_HidingDoor;
	void* m_DebugContext;
	void* m_Vocalization;
	void* m_LookAt;
	void* m_NarrowObject;
	void* m_Swim;
	void* m_UpperBody;
	void* m_RagdollGround;
	void* m_HumanCarriedBody;
	void* m_HumanBehaviorTransition;
	void* m_Elevator;
	void* m_HumanShipWheel;
	void* m_HumanSwivelGun;
	void* m_HumanShipRowBoat;
	void* m_HumanShipGunner;
	void* m_HumanShipSailingAssistant;
	void* m_HumanShipCannonOfficer;
	void* m_HumanClimbing;
	void* m_HumanHanging;
	void* m_HumanLockpick;
	void* m_HumanBoardgame;
	void* m_HumanBocce;
	void* m_HumanShipCustomAction;
	void* m_Harpoon;
	void* m_HumanShipLookOut;
	void* m_HumanShipCannonOperator;
	void* m_HumanShipDeck;
	void* m_HumanHangRope;
};

class HumanData : public AIActorData
{
public:
	enum WeaponState
	{
		WeaponState_None = 0x0,
		WeaponState_Sheathed = 0x1,
		WeaponState_Grabbed = 0x2,
		WeaponState_Unsheathed = 0x3,
	};

	enum HoodState
	{
		HoodState_HoodUp = 0x0,
		HoodState_HoodDown = 0x1,
	};

	enum AnimSyncState
	{
		AnimSyncState_None = 0x0,
		AnimSyncState_Active = 0x1,
		AnimSyncState_ShouldFall = 0x2,
		AnimSyncState_ShouldDie = 0x3,
	};

	enum HumanDeathState
	{
		HumanDeathState_NotDead = 0x0,
		HumanDeathState_Agonizing = 0x1,
		HumanDeathState_KO = 0x2,
		HumanDeathState_Dead = 0x3,
		HumanDeathState_DeathHeight = 0x4,
	};

	enum HumanGroundMobility
	{
		HumanMobility_Normal = 0x0,
		HumanMobility_LayDownHurt = 0x1,
		HumanMobility_MAX = 0x2,
	};

	struct FeetOnGroundFlags
	{
		unsigned __int8 m_LeftFootIsOnGround : 1;
		unsigned __int8 m_LeftToeIsOnGround : 1;
		unsigned __int8 m_RightFootIsOnGround : 1;
		unsigned __int8 m_RightToeIsOnGround : 1;
	};

	struct Flags
	{
		__int8 m_UseStumbleRoll : 1;
		__int8 m_UseStumbleIntentionRotation : 1;
		__int8 m_EnableCollisionOnMove : 1;
		__int8 m_EnableBreakableCollision : 1;
		__int8 m_GentlePushedIgnored : 1;
		__int8 m_BumpIgnored : 1;
		__int8 m_IsAssassinResponsibleForStumble : 1;
		__int8 m_GetupFast : 1;
		__int8 m_BlendInHidingSpot : 1;
		__int8 m_DisableCollisionWhenSit : 1;
	};

	HookMath::Vector4 m_WaterNormal;
	HookMath::Vector4 m_Unk00;
	HookMath::Vector4 m_LastFrameGlobalPosition;
	HumanDataBundle* m_HumanDataBundle;
	Handle m_LeftHandObject;
	Handle m_RightHandObject;
	Handle m_LeftHandTargetObject;
	Handle m_RightHandTargetObject;
	Handle m_SwordSheathObject;
	WeaponState m_LeftHandWeaponState;
	WeaponState m_RightHandWeaponState;
	Handle m_CurrentWeapon;
	Handle m_CurrentSecondaryWeapon;
	HoodState m_HoodState;
	Entity* m_DbgLeftHand;
	Entity* m_DbgRightHand;
	float m_FootMeshOffset;
	float m_ToeMeshOffset;
	float m_Tolerance;
	Handle m_Vehicle;
	Handle m_DrivableShipWheel;
	Handle m_DrivableRowBoatControlPoint;
	AnimSyncState m_SyncState;
	Reference m_HeavyRainCycleBreakerCustomActionPack;
	HumanDeathState m_DeathState;
	HumanGroundMobility m_GroundMobility;
	Reference m_CycleBreakerCustomActionPack;
	Reference m_LightRainCycleBreakerCustomActionPack;
	Handle m_WaterEntity;
	Reference m_LightSnowCycleBreakerCustomActionPack;
	Reference m_HeavySnowCycleBreakerCustomActionPack;
	float m_MinTimeBetweenCycleBreakers;
	float m_MaxTimeBetweenCycleBreakers;
	float m_WaterHeight;
	float m_WaterDepth;
	float m_WaterActualDepth;
	float m_WaterWetLevel;
	float m_CustomWaterHeight;
	WaterType m_WaterType;
	float m_SnowDepth;
	bool m_WeaponIsLockedDown;
	FeetOnGroundFlags m_Feet_On_Ground_Flags;
	bool m_ShotWhileAgonizing;
	bool m_AutoTargetChange;
	bool m_GentlePushEnabled;
	bool m_UseIdleBreakers;
	bool m_UsingCustomWaterHeight;
	bool m_UseDeepCornicheEjection;
	bool m_UseFastClimb;
	bool m_SkipActionOnStateEntry;
	bool m_UseCarryBoxStumble;
	bool m_UseWaterWell;
	bool m_HasHood;
	bool m_CanClimbTrees;
	bool m_ManualTargetingEnabled;
	bool m_TieHandInFront;
	bool m_Unk01;
	InventoryContainer m_Inventory[38];
	Flags m_Flags;
	ActorStateID m_CurrentAirAssassinationType;
	ActorStateID m_CurrentTackleType;
	ComponentCache m_WeaponComponentCache;
	void* m_InitialStateData;
};

class Human : public AIActor
{
public:
	enum GlobalPositionForPathfindStatus
	{
		GlobalPositionForPathfindStatus_NotSet = 0x0,
		GlobalPositionForPathfindStatus_Entity = 0x1,
		GlobalPositionForPathfindStatus_Custom = 0x2,
	};

	enum EdgeUnbalanceFallType
	{
		EdgeUnbalanceFallType_Hollywood = 0x0,
		EdgeUnbalanceFallType_Balanced = 0x1,
	};

	class AttackedByAnimalState
	{
	public:
		enum eAttackState
		{
			eAttackState_None = 0x0,
			eAttackState_Locked = 0x1,
		};

		eAttackState m_State;
		Handle m_Attacker;
		int m_AnimalsAttacking;
	};

	void* IHuman;
	void* IActorDeath;
	void* ICharacterPoisoned;
	HumanData m_Data;
	void* m_HumanGuidance;
	PoisonType m_CurrentPoisonType;
	float m_CurrentPoisonTime;
	int m_Unk00;
	bool m_NPCTriedWakingUsUp;
	HookMath::Vector4 m_PoisonDartHitLocalDirection;
	BaseSmallArray<void*> m_RestrictionZonesForClimbing;
	ClimbingGuidanceData m_ClimbingGuidanceData;
	bool m_IsUsingCustomNavigation;
	bool m_UsingClimbIKAtFurtherDistance;
	EventListener m_Listener;
	__declspec(align(16)) EventCallback m_OddZoneEventCallback;
	__declspec(align(16)) EventCallback m_UnkCallback00;
	__declspec(align(16)) EventCallback m_UnkCallback01;
	__declspec(align(16)) EventCallback m_UnkCallback02;
	int m_OddZoneCounter;
	bool m_PreOddZoneEnterHoodWasUp;
	bool m_NeedToForceTheHoodDown;
	bool m_NeedToForceTheHoodUp;
	bool m_HoodFirstFrameUpdate;
	char padding_0000[264];
	Timer m_CheckForDivableRegionsTimer;
	bool m_IsInDivableRegions;
	GuidanceReport m_Beams;
	HumanContextBundle m_HumanContextBundle;
	BodyBanking m_BodyBanking;
	void* m_IKController;
	IKClimbControler2 m_IKClimbControler2;
	IKTargetController m_IKTargetController;
	BeamDetector m_BeamDetector;
	GuidanceHoleDetector m_HoleDetector;
	HumanBody m_HumanBody;
	Handle m_Grabber;
	ComponentCache m_RagdollComponent;
	EdgeClampingSettings m_EdgeClampingSettings;
	HookMath::Vector4 m_LastSignificantBumpingPos;
	HookMath::Vector4 m_LastSignificantBumpingNormal;
	HookMath::Vector4 m_GlobalPositionForPathfind;
	Timer m_GlobalPositionForPathfindResetTimer;
	GlobalPositionForPathfindStatus m_GlobalPositionForPathfindStatus;
	BaseSmallArray<void*> m_TargetZones;
	BaseSmallArray<void*> m_UnbalanceIgnoreList;
	BaseSmallArray<void*> m_HurtIgnoreList;
	void* m_ControllingClip;
	void* m_HumanUpperBody;
	Handle m_AppleInstance;
	EdgeUnbalanceFallType m_EdgeUnbalanceFallType;
	AirAssassinationOrigin m_AirAssassinationOrigin;
	unsigned int m_AttackOnGroundCount;
	unsigned int m_LandingFailureTypeMask;
	float m_ForwardJumpDetectionScale;
	float m_LastSignificantBumpingDist;
	WeaponType m_PickedUpWeapon;
	ClimbingTargetManager m_ClimbingTargetManager;
	JumpSourceType m_DetectionJumpSourceType;
	GapInfo m_NearestGap;
	GapInfo m_Gap;
	bool m_IsGapValid;
	bool m_GapCalculated;
	bool m_GlobalPositionForPathfindResetTimerIsSet;
	bool m_IsNearestGapValid;
	bool m_NearestGapCalculated;
	bool m_LedgeTestACII_15776;
	bool m_IsEdgeClampingActive;
	bool m_IsFightEdgeClampingActive;
	bool m_IsHurtEdgeClampingActive;
	bool m_IsEdgeClampingEnable;
	bool m_IsEdgeClampingZeroSpeed;
	bool m_EdgeClampUsedLastFrame;
	bool m_ForcedGapUpdate;
	bool m_IsOnUnreachableAreaByAI;
	bool m_IsOnUnreachableAreaByAICalculated;
	bool m_UseAnimatedCapsuleOffsetForJumpDown;
	BaseSmallArray<float[2]> m_AnimatedCapsuleRadiusKeys;
	Timer m_AnimatedCapsuleRadiusTimer;
	BaseSmallArray<float[2]> m_AnimatedCapsuleHeightKeys;
	Timer m_AnimatedCapsuleHeightTimer;
	bool m_UseSlowFreeStepReception;
	bool m_CheckHandPose;
	bool m_ApplePropEnabled;
	bool m_LastSignificantBumpingDataIsSet;
	bool m_IsInBumpingNoFallZone;
	bool m_BumpingActivatedFirstFrame;
	bool m_AssassinationIntentionIsSet;
	bool m_AssassinationQueuingFollowsNormalPath;
	bool m_SkipHandPoseCheck;
	bool m_IsPoisoned;
	bool m_WeaponTotallyOut;
	bool m_IsInTree;
	bool m_IsOnShip;
	Handle m_IsOnShipEntity;
	bool m_IsOnOcean;
	bool m_DoJumpToVoid;
	bool m_DetectVShapesInWalling;
	Timer m_ExitTreeTimer;
	Timer m_ExitShipTimer;
	Timer m_UpdateIsOnOceanTimer;
	Timer m_InsideFireTimer;
	HookMath::Vector4 m_ToFireVector;
	bool m_CheckFireFlag;
	BaseSmallArray<void*> m_FireProximityDatas;
	BaseSmallArray<void*> m_FireDamageDatas;
	Timer m_NoLootableTimer;
	ContactMaterialManager m_ContactMaterialManager;
	WaterLevelManager_Cache m_WaterLevelCache;
	void* m_WaterLevelTriangle;
	bool m_WaterLevelFindCached;
	int m_StalkingZoneCount;
	int m_StalkingZoneBlendedCount;
	int m_StalkingClipCount;
	int m_StalkingClipBlendedCount;
	int m_StalkingClipAllowFastStalkingCount;
	int m_DisturbedZoneCount;
	unsigned int m_UglyHackIsBeingCuriousedByDomesticAnimal;
	Timer m_UglyHackLastCurious;
	HookMath::Vector4 m_NewDesiredFacingDirection;
	HookMath::Vector4 m_DesiredFacingDirection;
	AttackedByAnimalState m_AttackedByAnimalState;


	inline HumanData::HoodState GetHoodState() { return m_Data.m_HoodState; }
	inline bool GetHasHood() { return m_Data.m_HasHood; }
	inline bool GetIsCrouched() { return m_StalkingZoneCount > 0 || m_StalkingClipCount > 0; }

	bool __fastcall PutHoodDown(bool a1, bool a2, int a3);
	bool __fastcall PutHoodUp(bool a1, bool a2, int a3);
	void SetHasHood(bool val) { m_Data.m_HasHood = val; }
	void SetIsCrouched(bool val) { m_StalkingZoneCount = val; m_StalkingClipCount = val; m_StalkingClipAllowFastStalkingCount = val; }
	int* GetCharacterComponent();
	ActionComponent* GetActionComponent();
	void* GetContextByID(int track, int contextID); //3 = DebugContext, track 1 = default
};