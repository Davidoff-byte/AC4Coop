#pragma once

enum LODLevel
{
	LODLEVEL_NOTVISIBLE = 0x0,
	LODLEVEL_VERYFAR = 0x1,
	LODLEVEL_FAR = 0x2,
	LODLEVEL_MID = 0x3,
	LODLEVEL_NEAR = 0x4,
	LODLEVEL_VERYNEAR = 0x5,
	LODLEVELCOUNT = 0x6,
};

enum DuplicateMode
{
	DuplicateMode_Clone = 0x0,
	DuplicateMode_Instanciate = 0x1,
	DuplicateMode_InstanceClone = 0x2,
};

enum InitType
{
	InitType_Load = 0x0,
	InitType_LoadAfterActivation = 0x1,
	InitType_Duplicate = 0x2,
	InitType_ToolCreate = 0x3,
	NumberOfInitTypes = 0x4,
	InitType_LoadMask = 0x1,
	InitType_LoadAfterActivationMask = 0x2,
	InitType_DuplicateMask = 0x4,
	InitType_ToolCreateMask = 0x8,
};

enum Damage
{
	Damage_Light = 0x0,
	Damage_Medium = 0x1,
	Damage_Heavy = 0x2,
	Damage_Super_Heavy = 0x3,
	Damage_None = 0x3E8,
};

enum CharacterInteractionMode
{
	CharacterInteraction_Blocking = 0x0,
	CharacterInteraction_Debris = 0x1,
	CharacterInteraction_Pushable = 0x2,
	CharacterInteraction_BlockingMobile = 0x3,
	CharacterInteraction_Ignore = 0x4,
};

enum GuidanceObjectSubType
{
	GuidanceObjectSubTypeNone = 0x0,
	GuidanceObjectSubTypeLedgeGrab = 0x1,
	GuidanceObjectSubTypeBeam = 0x2,
	GuidanceObjectSubTypeLadder = 0x3,
	GuidanceObjectSubTypePole = 0x4,
	GuidanceObjectSubTypeRope = 0x5,
	GuidanceObjectSubTypeSurface = 0x6,
	GuidanceObjectSubTypeKiosk = 0x7,
	GuidanceObjectSubTypeBoatPlank = 0x8,
	GuidanceObjectSubTypeBoatInnerEndEdge = 0x9,
	GuidanceObjectSubTypeBoatInnerSideEdge = 0xA,
	GuidanceObjectSubTypeBoatMiddleEdge = 0xB,
	GuidanceObjectSubTypeHayStack = 0xC,
	GuidanceObjectSubTypeLedgeGrabHandsOnly = 0xD,
	GuidanceObjectSubTypeFork_Hangfork = 0xE,
	GuidanceObjectSubTypeFork_Hangwall = 0xF,
	GuidanceObjectSubTypeParallel = 0x10,
	GuidanceObjectSubTypeHorizontalParallel = 0x11,
	GuidanceObjectSubTypeLedgeGrabPole = 0x12,
	GuidanceObjectSubTypePassOver = 0x13,
	GuidanceObjectSubTypeUnderwaterRope = 0x14,
	GuidanceObjectSubTypeHorizontalRope = 0x15,
	GuidanceObjectSubTypeLedgeLookDown = 0x16,
};

enum FreeStepEndPose
{
	FreeStepEndPose_Unset = 0xFFFFFFFF,
	FreeStepEndPose_Low = 0x0,
	FreeStepEndPose_Medium = 0x1,
	FreeStepEndPose_MAX = 0x2,
};

enum ePickableType
{
	ePickableType_Chest = 0x0,
	ePickableType_AliveBody = 0x1,
	ePickableType_DeadBody = 0x2,
	ePickableType_Lantern = 0x3,
	ePickableType_Keg = 0x4,
	ePickableType_NUM = 0x5,
	ePickableType_Invalid = 0x6,
};

enum WeaponType
{
	WeaponType_Unarmed = 0x0,
	WeaponType_Medium = 0x1,
	WeaponType_Small = 0x2,
	WeaponType_Heavy = 0x3,
	WeaponType_Long = 0x4,
	WeaponType_Musket = 0x5,
	WeaponType_Blunt = 0x6,
	WeaponType_DualWieldPrimary = 0x7,
	WeaponType_DualWieldSecondary = 0x8,
	WeaponType_Machete = 0x9,
	WeaponType_Lantern = 0xA,
	WeaponType_HiddenBlade = 0xB,
	WeaponType_MediumBlunt = 0xC,
	WeaponType_Crossbow = 0xD,
	WeaponType_RightHolsterPistol = 0xE,
	WeaponType_Bow = 0xF,
	WeaponType_ThrowingDagger = 0x10,
	WeaponType_Arrow = 0x11,
	WeaponType_Blowpipe = 0x12,
	WeaponType_BlowpipeNormalDart = 0x13,
	WeaponType_BlowpipeSleepDart = 0x14,
	WeaponType_BlowpipeBerserkDart = 0x15,
	WeaponType_Rock = 0x16,
	WeaponType_Gun = 0x17,
	WeaponType_AppleOfEden = 0x18,
	WeaponType_RopeDart = 0x19,
	WeaponType_LeftHolsterPistol = 0x1A,
	WeaponType_Explosive = 0x1B,
	WeaponType_LeftHandKnife = 0x1C,
	WeaponType_SnitchDrum = 0x1D,
	WeaponType_LeftUpHolsterPistol = 0x1E,
	WeaponType_RightUpHolsterPistol = 0x1F,
	WeaponType_SwivelGun = 0x20,
	WeaponType_Shotgun = 0x21,
	WeaponType_MaxWeaponType = 0x22,
};

enum HurtDirection
{
	HurtDirection_Front = 0x0,
	HurtDirection_Back = 0x1,
	HurtDirection_Left = 0x2,
	HurtDirection_Right = 0x3,
	HurtDirection_MAX = 0x4,
};

enum HurtFightDirection
{
	HurtFightDirection_Left = 0x0,
	HurtFightDirection_Right = 0x1,
	HurtFightDirection_BackLeft = 0x2,
	HurtFightDirection_BackRight = 0x3,
	HurtFightDirection_MAX = 0x4,
};

enum IncapacitateRating
{
	IncapacitateRating_Small = 0x0,
	IncapacitateRating_Medium = 0x1,
	IncapacitateRating_Big = 0x2,
	IncapacitateRating_MAX = 0x3,
};

enum HitLocation
{
	HitLocation_Body = 0x0,
	HitLocation_Head = 0x1,
	HitLocation_LeftArm = 0x2,
	HitLocation_RightArm = 0x3,
	HitLocation_LeftLeg = 0x4,
	HitLocation_RightLeg = 0x5,
	HitLocation_MAX = 0x6,
};

enum DodgeType
{
	DodgeType_Auto = 0x0,
	DodgeType_Short = 0x1,
	DodgeType_Long = 0x2,
};

enum PinDownSlotType
{
	PinDownSlotType_NotSet = 0x0,
	PinDownSlotType_LeftArm = 0x1,
	PinDownSlotType_RightArm = 0x2,
	PinDownSlotType_Front = 0x3,
	PinDownSlotType_Back = 0x4,
};

enum JumpType
{
	JumpType_Straight = 0x0,
	JumpType_Release = 0x1,
	JumpType_Release_Targeted = 0x2,
	JumpType_Straight_Targeted = 0x3,
	JumpType_Horizontal_Targeted = 0x4,
};

enum InAirSubState
{
	InAirSubState_None = 0x0,
	InAirSubState_TurnOnWall = 0x1,
	InAirSubState_ReboundTransition = 0x2,
};

enum WaterType
{
	WaterType_ClearWater = 0x0,
	WaterType_Ocean = 0x1,
	WaterType_Divable = 0x2,
	WaterType_Unknown = 0x3,
};

enum PoisonType
{
	PoisonType_None = 0x0,
	PoisonType_Sleep = 0x1,
	PoisonType_Berserk = 0x2,
};

enum AirAssassinationOrigin
{
	AirAssassinationOrigin_None = 0x0,
	AirAssassinationOrigin_HayStack = 0x1,
	AirAssassinationOrigin_Ledge = 0x2,
	AirAssassinationOrigin_Bench = 0x3,
	AirAssassinationOrigin_WaterWell = 0x4,
	AirAssassinationOrigin_SwingRope = 0x5,
};

enum JumpSourceType
{
	JumpSourceType_Ground = 0x0,
	JumpSourceType_FreeStep = 0x1,
	JumpSourceType_Wall = 0x2,
	JumpSourceType_Swing = 0x3,
	JumpSourceType_HandPassOver = 0x4,
	JumpSourceType_SwingHighMomentum = 0x5,
	JumpSourceType_VStep = 0x6,
	JumpSourceType_Rope = 0x7,
	JumpSourceType_Invalid = 0x8,
};

enum LookAtRoot
{
	LookAtRoot_Eyes = 0x0,
	LookAtRoot_Head = 0x1,
	LookAtRoot_Body = 0x2,
	LookAtRoot_All = 0x3,
};

enum IDSerializationType
{
	IDST_Normal = 0x0,
	IDST_Stable = 0x1,
	IDST_NoID = 0x2,
};

enum ShipSide
{
	ShipSide_Unset = 0xFFFFFFFF,
	ShipSide_Left = 0x0,
	ShipSide_Right = 0x1,
	ShipSide_Front = 0x2,
	ShipSide_Back = 0x3,
	ShipSide_Count = 0x4,
};

enum NavalAmmoType
{
	NavalAmmoType_RoundShot = 0x0,
	NavalAmmoType_ChainShot = 0x1,
	NavalAmmoType_GrapeShot = 0x2,
	NavalAmmoType_GrapplingHook = 0x3,
	NavalAmmoType_PiercingShot = 0x4,
	NavalAmmoType_NavalMortar = 0x5,
	NavalAmmoType_SmokeShot = 0x6,
	NavalAmmoType_HeavyShot = 0x7,
	NavalAmmoType_NavalMine = 0x8,
	NavalAmmoType_BlankShot = 0x9,
	NavalAmmoType_HarpoonRope = 0xA,
	NavalAmmoType_HarpoonNoRope = 0xB,
	NavalAmmoType_Empty = 0xC,
	NavalAmmoType_Cluster = 0xD,
	NavalAmmoType_Count = 0xE,
};
