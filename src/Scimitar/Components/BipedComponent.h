#pragma once

#include "CharacterController.h"
#include "..\ObjectShape.h"

class BipedComponent : public CharacterController
{
public:
	CapsuleShape m_CapsuleShape;

public:
	inline static ClassDescriptor* GetStaticClassDescriptor()
	{
		return Hook::CallCdeclMethod<ClassDescriptor*>(0x00423D30);
	}

	inline int SetEnabled(bool enabled)
	{
		return Component::SetEnabled(enabled);
	}

	inline CollisionFilterInfo* GetCollisionFilterInfo()
	{
		return (CollisionFilterInfo*)&m_FilterInfo;
	}
};
