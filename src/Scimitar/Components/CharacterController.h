#pragma once

#include "..\BaseTypes.h"
#include "..\..\HookMath.h"
#include "PhysicComponent.h"

class CharacterController : public PhysicComponent
{
public:
	HookMath::Vector4 m_Gravity;
	HookMath::Vector4 m_LocalOffset;
	HookMath::Vector4 m_Offset;
	float m_Mass;
	float m_InertAllowedPenetration;
	float m_RigidBodyAllowedPenetration;
	float m_CharacterAllowedPenetration;
	float m_StickingDistance;
	float m_BumpingDistance;
	float m_Radius;
	float m_Height;
	HookMath::Vector3 m_Rotation;
	float m_DynamicFriction;
	CollisionFilterInfo m_FilterInfo;
	bool m_OrientVelocityOnSlope;
	bool m_UseGravity;
	bool m_UseSticking;
	bool m_UseBumping;
	HookMath::Vector4 m_Velocity;
	HookMath::Vector4 m_ExtraDisplacement;
	HookMath::Vector4 m_LastExtraDisplacement;
	HookMath::Vector4 m_LocalOffsetStartPos;
	HookMath::Vector4 m_LocalOffsetEndPos;
	HookMath::Vector4 m_LastSetPositionNoOffset;
	HookMath::Vector4 m_ClientVelocity[2];
	HookMath::Vector4 m_ClientExtraVelocity[2];
	HookMath::Vector4 m_Force;
	HookMath::Vector4 m_Impulse;
	HookMath::Vector4 m_VelocityBeforeContact;
	HookMath::Vector4 m_RequestedVelocity;
	HookMath::Vector4 m_VelocityFromLocalOffset;
	HookMath::Vector4 m_VelocityPosition;
	HookMath::Vector4 m_KamikaziPosition;
	HookMath::Quaternion m_VelocityOrientation;
	HookMath::Quaternion m_KamikaziOrientation;
	HookMath::Vector4 m_LastEntityPosition;
	HookMath::Vector4 m_InterpolatorResult;
	HookMath::Vector4 m_PreCastPosition;
	HookMath::Vector4 m_OldDisplacement;
	BYTE m_ContactPointData[640];
	ContactPoint m_StickingContact;
	Timer m_LocalOffsetTimer;
	BaseSmallArray<void> m_Manifold;
	BaseSmallArray<void> m_NonManifoldContacts;
	BaseSmallArray<void> m_LocalOffsetKeys;
	void* m_GroupPhysicZoneData;
	void* m_EntityDisplacementDataList;
	void* m_ShapePhantom;
	void* m_DisplacementManager;
	float m_MaxSlopeCosine;
	float m_MaxSlopeTangent;
	float m_StaticFriction;
	float m_ExtraUpStaticFriction;
	float m_ExtraDownStaticFriction;
	float m_PenetrationRecoverySpeed;
	bool m_PauseHierarchyChanges;
	float m_LastBumpingCorrection;
	float m_RemainingTime;
	bool m_HaveStickingContact;
	bool m_TransformWasSet;
	bool m_MustRefreshManifold;
	bool m_ClientHasCalledSetVelocity[2];
	bool m_ClientHasCalledSetExtraVelocity[2];
	bool m_ConcatenateToAnim;
	bool m_DoLinearCasts;
	bool m_UseVelocityPosition;
	bool m_UseVelocityOrientation;
	bool m_UseKamikazePosition;
	bool m_UseKamikazeOrientation;
	bool m_HollywoodMode;
	bool m_IgnorePositionInSetMatrixHACK;
	bool m_InUpdateAfterSimulation123;
	bool m_IsDisabled;
	bool m_IsNavalWorld;
	HookMath::Vector4 m_ShapeOffsetStartPos;
	HookMath::Vector4 m_ShapeOffsetEndPos;
	Timer m_ShapeOffsetTimer;
	BaseSmallArray<void> m_ShapeOffsetKeys;
	BaseSmallArray<void> m_ExtraDisplacementFunctorList;
	HookMath::Vector4 m_UnknownVector;

public:
	inline static int* GetStaticClassDescriptor()
	{
		return Hook::CallCdeclMethod<int*>(0x007AA8E0);
	}

	inline int SetEnabled(bool enabled)
	{
		return Component::SetEnabled(enabled);
	}
};
