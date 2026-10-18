#include "Entity.h"

// Entity entry points. The ones that matter to the mod are SetGlobalMatrixInternal (teleport/pose drive,
// with the physics flag choosing between a plain write and a capsule push) and GetComponentByType (every
// component lookup goes through it rather than the raw array).

int Entity::Duplicate()
{
	return Hook::CallClassMethod<int, Entity*, int*, int>(0x0052A980, this, 0, 1);
}

void Entity::SetIsHidden(int hidden)
{
	Hook::CallFastCallMethod<void, int, int, int>(0x0050EA80, (int)this, 0, hidden);
}

Component* Entity::GetComponentByType(ClassDescriptor* componentDescriptor)
{
	return Hook::CallClassMethod<Component*, Entity*, ClassDescriptor*>(0x005200C0, this, componentDescriptor);
}

void Entity::FlagsChanged()
{
	Hook::CallCdeclMethod<Entity*>(0x0050E8A0, this);
}

void Entity::ShadowFlagsChanged()
{
	Hook::CallCdeclMethod<Entity*>(0x0050A6D0, this);
}

BaseEntity* Entity::GetHighestParentEntity()
{
	return Hook::CallClassMethod<BaseEntity*, Entity*>(0x0051D580, this);
}

void* Entity::GetBehaviorComponent()
{
	return Hook::CallClassMethod<void*, Entity*>(0x005220D0, this);
}

void Entity::OnAddToWorld(void* world)
{
	Hook::CallFastCallMethod<void, Entity*, void*>(0x5131F0, this, world);
}

// Nothing to tell the renderer when there is no graphic node yet.
void Entity::ReflectionFlagsChanged()
{
	if (m_GraphicNode)
	{
		Hook::CallClassMethod<void*>(0x0087FFD0, m_GraphicNode);
	}
}

// Writing the matrix directly is enough for a teleport; updatePhysics makes the engine push the capsule
// through the world so collision and height follow.
void Entity::SetGlobalMatrixInternal(HookMath::Transform* matrix, bool updatePhysics)
{
	if (!matrix)
	{
		return;
	}

	Hook::CallClassMethod<void, Entity*, HookMath::Transform*, bool>(0x00528200, this, matrix, updatePhysics);
}

BaseEntity* BaseEntity::GetFatherEntity()
{
	return Hook::CallClassMethod<BaseEntity*, BaseEntity*>(0x00639E00, this);
}

int BaseEntity::SetGlobalRotation(HookMath::Quaternion* rot)
{
	return Hook::CallClassMethod<int, BaseEntity*, HookMath::Quaternion*>(0x004314E0, this, rot);
}

char BaseEntity::SetSight(HookMath::Vector4* sight, HookMath::Vector4* banking)
{
	return Hook::CallClassMethod<char, BaseEntity*, HookMath::Vector4*, HookMath::Vector4*>(0x0063C310, this, sight, banking);
}
