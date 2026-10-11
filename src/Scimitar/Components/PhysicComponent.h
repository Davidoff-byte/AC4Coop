#pragma once

#include "..\BaseTypes.h"
#include "..\CollisionFilterInfo.h"
#include "..\..\HookMath.h"
#include "Component.h"

enum Resistance
{
	Resistance_Fragile = 0x0,
	Resistance_Normal = 0x1,
	Resistance_Solid = 0x2,
	Resistance_Indestructible = 0x3,
};

class PhysicComponent : public Component
{
public:
	Damage m_Damage;
	CharacterInteractionMode m_CharacterInteractionMode;
	bool m_Zombied;
	bool m_DriveEntity;
	CollisionReport m_CollisionReport;
	void* m_AvoidanceObject;
	void* m_PhysicHierarchy;
	PhysicComponent* m_PhysicsOwner;
	Handle m_HierarchyFatherHandle;

public:
	inline static ClassDescriptor* GetStaticClassDescriptor()
	{
		return Hook::CallCdeclMethod<ClassDescriptor*>(0x00735670);
	}

	inline int SetEnabled(bool enabled)
	{
		return Component::SetEnabled(enabled);
	}

	inline bool GetEnabled()
	{
		return (m_ObjectFlags & 0x4000000) != 0;
	}
};

class InteractivePhysicComponent : public PhysicComponent
{
public:
	Resistance m_Resistance;
	Reference m_ImpactData;
};

class RigidBody : public Object
{
public:
	enum DrivingMode
	{
		Fixed = 0x0,
		KeyFramed = 0x1,
		Dynamic_0 = 0x2,
	};

	class Flags
	{
		BYTE m_IsNailed : 1;
		BYTE m_ClearVelocityOnReInit : 1;
		BYTE m_WorldBatchOperationFlag : 2;
	};

	HookMath::Transform m_InertiaTensorLocal;
	Reference m_Shape;
	float m_Mass;
	float m_LinearDamping;
	float m_AngularDamping;
	DrivingMode m_DrivingMode;
	CollisionFilterInfo m_FilterInfo;
	Flags m_RigidBodyFlags;
	void* m_HKRigidBody;
	void* ForceAccumulator;
};

class InertComponent : public PhysicComponent
{
public:
	RigidBody m_RigidBody;
	void* m_GuidanceSystemPtr;
	GameplaySurfaceNavType m_GPSurfaceNavType;
	BYTE m_PackedIsGround : 1;
	BYTE m_PackedIsMerged : 1;

	inline static ClassDescriptor* GetStaticClassDescriptor()
	{
		return Hook::CallCdeclMethod<ClassDescriptor*>(0x61F790);
	}
};

class RigidBodyComponent : public InteractivePhysicComponent
{
public:
	class Flags
	{
		BYTE m_ForceAvoidanceSystemRegistration : 1;
		BYTE m_ShowRigidBodies : 1;
		BYTE m_UseGravityWhenKeyFramed : 1;
		BYTE m_HasPhysicsInitTransform : 1;
		BYTE m_AttachToKeyframeShip : 1;
	};

	HookMath::Transform m_EntityInitTransform;
	HookMath::Vector4 m_LinearConstraint;
	HookMath::Vector4 m_AngularConstraint;
	RigidBody m_RigidBody;
	float m_MaxLinearVelocity;
	float m_MaxAngularVelocity;
	GameplaySurfaceNavType m_GPSurfaceNavType;
	bool m_UpdateEntityBV;
	Flags m_RBCFlags;
	bool m_AutoUpdatePhysicHierarchy;
	HookMath::Vector4 m_LinearVelocityFromAnimation;
	HookMath::Vector4 m_AngularVelocityFromAnimation;
	uint32_t m_KillFrameCounter;
	bool m_AddedToPostSimulationCallback;
	bool m_PermitAddToWorld;
	bool m_ManuallyRemovedFromWorld;
	bool m_AddedToUpdateList;

	inline static ClassDescriptor* GetStaticClassDescriptor()
	{
		return Hook::CallCdeclMethod<ClassDescriptor*>(0x7357E0);
	}
};
