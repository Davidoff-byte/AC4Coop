#include "BoneHandle.h"

HookMath::Vector4 BoneHandle::GetPosition()
{
	HookMath::Vector4 output;

	Hook::CallClassMethod<HookMath::Vector4*, BoneHandle*, HookMath::Vector4*>(0x00423110, this, &output);

	return output;
}

HookMath::Quaternion BoneHandle::GetRotation()
{
	HookMath::Quaternion output;

	Hook::CallClassMethod<HookMath::Vector4*, BoneHandle*, HookMath::Quaternion*>(0x0049DEC0, this, &output);

	return output;
}

void BoneHandle::GetTransform(HookMath::Transform* output)
{
	Hook::CallClassMethod<HookMath::Vector4*, BoneHandle*, HookMath::Transform*>(0x004A1C90, this, output);
}

bool BoneHandle::HasFather()
{
	return Hook::CallClassMethod<bool, BoneHandle*>(0x004AEC60, this);
}

BYTE BoneHandle::GetFatherIndex()
{
	return Hook::CallClassMethod<BYTE, BoneHandle*>(0x004AEC20, this);
}

BYTE BoneHandle::GetFatherIndexWithComponent(int* comp)
{
	return Hook::CallClassMethod<BYTE, BoneHandle*, int*>(0x004A2610, this, comp);
}

BoneHandle* BoneHandle::GetFather()
{
	BoneHandle* out = new(BoneHandle);

	if (!out)
	{
		return NULL;
	}

	Hook::CallClassMethod<BoneHandle*, BoneHandle*, BoneHandle*>(0x004AEBC0, this, out);

	return out;
}

BYTE BoneHandle::GetChildBoneCount()
{
	return Hook::CallClassMethod<BYTE, BoneHandle*>(0x004A2530, this);
}

unsigned int BoneHandle::GetBoneID()
{
	return Hook::CallClassMethod<unsigned int, BoneHandle*>(0x49DB10, this);
}

void BoneHandle::SetLocalPositionWithComponent(Component* component, HookMath::Vector4* value)
{
	return Hook::CallClassMethod<void, BoneHandle*, Component*, HookMath::Vector4*>(0x004A2CB0, this, component, value);
}

void BoneHandle::SetLocalRotationWithComponent(Component* component, HookMath::Quaternion* value)
{
	return Hook::CallClassMethod<void, BoneHandle*, Component*, HookMath::Quaternion*>(0x004A2D50, this, component, value);
}

void BoneHandle::SetGlobalPositionWithComponent(Component* component, HookMath::Vector4* value)
{
	return Hook::CallClassMethod<void, BoneHandle*, Component*, HookMath::Vector4*>(0x004A2B70, this, component, value);
}

HookMath::Vector4 BoneHandle::GetGlobalPosition(HookMath::Vector4* value)
{
	return Hook::CallClassMethod<HookMath::Vector4, BoneHandle*, HookMath::Vector4*>(0x0049DE60, this, value);
}

HookMath::Quaternion BoneHandle::GetGlobalRotation(HookMath::Quaternion* value)
{
	return Hook::CallClassMethod<HookMath::Quaternion, BoneHandle*, HookMath::Quaternion*>(0x0049DEC0, this, value);
}

int BoneHandle::SetGlobalRotation(HookMath::Quaternion* value)
{
	return Hook::CallClassMethod<int, BoneHandle*, HookMath::Quaternion*>(0x004BB2B0, this, value);
}