#pragma once

#include "BaseTypes.h"
#include "..\HookMath.h"
#include "CollisionFilterInfo.h"

enum ContactMaterial
{
	Dirt = 0x0,
	Mud = 0x1,
	Sand = 0x2,
	Pebbles = 0x3,
	Stone_Clean = 0x4,
	Stone_Dirt = 0x5,
	Marble = 0x6,
	Ceramic = 0x7,
	Wood_Solid = 0x8,
	Wood_Plank = 0x9,
	WhiteRoom = 0xA,
	Wood_Creak = 0xB,
	Carpet = 0xC,
	Straw = 0xD,
	Grass_Short = 0xE,
	Grass_Long = 0xF,
	Leaves = 0x10,
	Leaves_Branch = 0x11,
	Water_Puddle = 0x12,
	Water_Shallow = 0x13,
	Snow = 0x14,
	Glass = 0x15,
	Concrete_Polished = 0x16,
	Metal_Dome = 0x17,
	Metal_Grill = 0x18,
	Bush = 0x19,
	Vines = 0x1A,
	Stone_Fort = 0x1B,
	Snow_Deep = 0x1C,
	Snow_Low = 0x1D,
	TreeBranch = 0x1E,
	Water_Deep = 0x1F,
	Rope_Material = 0x20,
	ContactMaterial_NormalCount = 0x21,
	ContactMaterial_Overrider = 0xFA0,
	ContactMaterial_Default = 0xFA1,
	ContactMaterial_Invalid = 0xFFFFFFFF,
};

class CollisionMaterial : public ManagedObject
{
public:
	enum GraphicalType
	{
		GraphicalNone = 0x0,
		GraphicalImpactFX = 0x1,
	};

	enum AIType
	{
		AIType_None = 0x0,
		AIType_UnreachableAreaByAI = 0x1,
		AIType_StepOff = 0x2,
		AIType_Damage = 0x3,
		AIType_Stalk = 0x4,
	};

	ContactMaterial m_SoundType;
	GraphicalType m_GraphicalType;
	AIType m_AIType;
	int m_MaterialDamage;
	float m_Friction;
	float m_Restitution;
	CollisionFilterInfo m_CollisionFilterInfo;
	IColor m_Color;
};

class ObjectShape : public ManagedObject
{
public:
	UserResourceCategory m_UserCategory;
};

class HKPCapsule
{
public:
	void* vtbl;
	char padding[8];
	ObjectShape* m_Shape;
	float m_Radius;
	HookMath::Vector4 m_Bottom;
	HookMath::Vector4 m_Top;
};

class HKPBox
{
public:
	void* vtbl;
	char padding_0000[8];
	ObjectShape* m_Shape;
	char padding_0001[16];
	HookMath::Vector4 m_HalfExtents;
};

class __declspec(align(16)) HKPSphere
{
public:
	void* vtbl;
	char padding_0000[8];
	ObjectShape* m_Shape;
	char padding_0001[16];
	float m_Radius;
};

class __declspec(align(16)) HKPCylinder
{
public:
	void* vtbl;
	char padding_0000[8];
	ObjectShape* m_Shape;
	float m_BoundingVolumeRadius;
	float m_CylBaseRadiusFactorForHeightFieldCollisions;
	HookMath::Vector4 m_Bottom;
	HookMath::Vector4 m_Top;
};

class HKPConvexTransform
{
public:
	void* vtbl;
	char padding_0000[8];
	ObjectShape* m_Shape;
	char padding_0001[16];
	HookMath::PosQuat m_Transform;
};

class MeshShapeTriangleMaterialData
{
public:
	Reference m_Material;
	CollisionFilterInfo m_FilterInfo;
};

class CapsuleShape : public ObjectShape
{
public:
	BaseBigArray<HookMath::Vector3> m_Vertices;
	BaseBigArray<WORD> m_IndicesTriangles;
	BaseBigArray<BYTE> m_IndicesMaterialData;
	Reference m_Material;
	BaseSmallArray<MeshShapeTriangleMaterialData> m_TriangleMaterialData;
	HKPCapsule* m_Capsule;
};

class BoxShape : public ObjectShape
{
public:
	Reference m_Material;
	HKPBox* m_Box;
	HKPConvexTransform* m_TransformShape;
};

class SphereShape : public ObjectShape
{
public:
	Reference m_Material;
	HKPSphere* m_Sphere;
	HKPConvexTransform* m_TransformShape;
};

class CylinderShape : public ObjectShape
{
public:
	Reference m_Material;
	HKPCylinder* m_Cylinder;
};

class MeshShape : public ObjectShape
{
public:
	HookMath::Vector4 m_MoppOffsetAndScale;
	HookMath::Vector4 m_MinLocalAABBox;
	HookMath::Vector4 m_MaxLocalAABBox;
	BaseBigArray<BYTE> m_MoppCode;
	BaseBigArray<HookMath::Vector3> m_Vertices;
	BaseBigArray<WORD> m_IndicesTriangles;
	BaseBigArray<BYTE> m_IndicesMaterialData;
	BaseSmallArray<MeshShapeTriangleMaterialData> m_TriangleMaterialData;
	bool m_OneCollisionMaterialOnly;
	BYTE m_MaterialUniqeIndex;
	void* m_Mesh;
	void* m_Mopp;
};