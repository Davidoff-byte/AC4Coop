#pragma once

#include "BaseTypes.h"
#include "Entity.h"

// Reference to one bone of a skeleton. The engine constructor (0x49DA40) stores the shared invalid
// handle container at +0 and 0xFF at +4, so the object is 5 bytes: a Handle plus a byte index.
// Human and InventorySlot embed it by value, so the order and the types must not change.
class BoneHandle
{
public:
	BoneHandle()
	{
		Hook::CallClassMethod<BoneHandle*, BoneHandle*>(0x0049DA40, this);
	}

	Handle m_SkeletonComponent;
	BYTE m_BoneIndex = 255;

	// Every method below is a direct thunk to an engine routine; none of them is called from this
	// project today, they are declared so the class keeps its shape and stays complete.
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
