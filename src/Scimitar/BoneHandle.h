#pragma once

#include "BaseTypes.h"
#include "Entity.h"

class BoneHandle
{
public:
	BoneHandle()
	{
		Hook::CallClassMethod<BoneHandle*, BoneHandle*>(0x0049DA40, this);
	}

	Handle m_SkeletonComponent;
	BYTE m_BoneIndex = 255;

	HookMath::Vector4 GetPosition();
	HookMath::Quaternion GetRotation();
	void GetTransform(HookMath::Transform* output);
	bool HasFather();
	BYTE GetFatherIndex();
	BYTE GetFatherIndexWithComponent(int* comp);
	BoneHandle* GetFather();
	BYTE GetChildBoneCount();
	unsigned int GetBoneID();
	void SetLocalPositionWithComponent(Component* component, HookMath::Vector4* value);
	void SetLocalRotationWithComponent(Component* component, HookMath::Quaternion* value);
	void SetGlobalPositionWithComponent(Component* component, HookMath::Vector4* value);
	HookMath::Vector4 GetGlobalPosition(HookMath::Vector4* value);
	HookMath::Quaternion GetGlobalRotation(HookMath::Quaternion* value);
	int SetGlobalRotation(HookMath::Quaternion* value);
};
