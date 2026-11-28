#include "CollisionFilterInfo.h"

bool CollisionFilterInfo::SetLayer(int layer)
{
	return Hook::CallClassMethod<bool, CollisionFilterInfo*, int>(0x00636480, this, layer);
}

uint64_t CollisionQueryCollisionFilter::GetCollisionsEnabledAsMask(CollisionFilterInfo::Layer layer)
{
	return Hook::CallCdeclMethod<uint64_t, CollisionFilterInfo::Layer>(0x7CCAF0, layer);
}

bool RayCast::RayCastHitTest(World* world, void* physicWorkspace, ContactPoint* outContactPoint, Entity* excludedEntity, HookMath::Vector4* rayStart, HookMath::Vector4* rayEnd, uint64_t layerQueryMask, bool collectBackFace, bool useWallNormal, bool useGroundNormal)
{
	return Hook::CallCdeclMethod<bool, World*, void*, ContactPoint*, Entity*, HookMath::Vector4*, HookMath::Vector4*, uint64_t, bool, bool, bool>(0x11084E0, world, physicWorkspace, outContactPoint, excludedEntity, rayStart, rayEnd, layerQueryMask, collectBackFace, useWallNormal, useGroundNormal);
}

Entity* RayCast::RayCastHitEntity(void* physicWorkspace, Entity* excludedEntity, HookMath::Vector4* rayStart, HookMath::Vector4* rayEnd, uint64_t layerQueryMask, bool collectBackFace, bool useWallNormal, bool useGroundNormal)
{
	static ContactPoint contactPoint = ContactPoint();

	if (World* world = World::GetCurrentWorld())
	{
		if (RayCast::RayCastHitTest(world, physicWorkspace, &contactPoint, excludedEntity, rayStart, rayEnd, layerQueryMask, collectBackFace, useWallNormal, useGroundNormal))
		{
			if (contactPoint.m_PhysicComponent.m_Component)
			{
				return contactPoint.m_PhysicComponent.m_Component->m_Owner;
			}
		}
	}

	return nullptr;
}