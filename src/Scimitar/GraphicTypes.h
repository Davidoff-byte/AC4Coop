#pragma once

#include "BaseTypes.h"
#include "..\HookMath.h"

// Rendering-side types. Entity embeds BoundingVolume and GraphicNodeUpdateTag by value, so their sizes
// decide every field after them; the rest of the file is enums whose values come from the engine data.

class PoolAddress
{
public:
	BYTE m_IndexInPool;
	BYTE m_PoolIndex;
};

class GraphicNodeUpdateTag
{
public:
	int m_FrameCounter;
	int m_Index;
};

class GraphicNode
{
public:
	class Flags
	{
	public:
		unsigned __int32 m_GraphicNodeType : 4;
		unsigned __int32 m_NodeFilterMask : 11;
		unsigned __int32 m_LightsValid : 1;
		unsigned __int32 m_BreakOnNode : 1;
		unsigned __int32 m_SweepDistance : 9;
	};

	HookMath::Vector4 m_Min;
	HookMath::Vector4 m_Max;
	unsigned int m_CullResultMasks;
	unsigned int m_LastDrawn;
	unsigned int m_LastRejected;
	Flags m_Flags;
	PoolAddress m_NodePoolAddress;
	WORD m_IndexInSector;
};

// 32 bytes: two Vector3 plus the type and the optional parent matrix.
class BoundingVolume
{
public:
	enum BVType
	{
		AABBox = 0x0,
		Sphere = 0x1,
		OOBBox = 0x2,
	};

	HookMath::Vector3 m_Min;
	HookMath::Vector3 m_Max;
	BVType m_Type;
	HookMath::Transform* m_ParentMatrix;
};

class IColor
{
public:
	BYTE m_R;
	BYTE m_G;
	BYTE m_B;
	BYTE m_A;
};

class Color
{
public:
	float m_A;
	float m_R;
	float m_G;
	float m_B;
};

class HWBuffer
{
public:
	enum HWBufferType
	{
		HWBT_VertexBuffer = 0x0,
		HWBT_IndexBuffer = 0x1,
		HWBT_TextureBuffer = 0x2,
	};

	void* m_HWBuffer;
	unsigned int m_BufferSize;
	HWBufferType m_Type;
	BaseSmallArray<BYTE> m_Header;
};

enum BlendMode
{
	Blend_Copy = 0x0,
	Blend_Additive = 0x1,
	Blend_Substractive = 0x2,
	Blend_Modulate = 0x3,
	Blend_Alpha = 0x4,
	Blend_Dodge = 0x5,
	Blend_AlphaAdditive = 0x6,
	Blend_DestAlpha = 0x7,
	NbBlendModes = 0x8,
};

enum PixelFormat
{
	PixelFormat_RGBA8888 = 0x0,
	PixelFormat_RGBA8888Signed = 0x1,
	PixelFormat_DXT1 = 0x2,
	PixelFormat_DXT1A = 0x3,
	PixelFormat_DXT3 = 0x4,
	PixelFormat_DXT5 = 0x5,
	PixelFormat_DXN = 0x6,
	PixelFormat_A8 = 0x7,
	PixelFormat_I8 = 0x8,
	PixelFormat_I16 = 0x9,
	PixelFormat_A8I8 = 0xA,
	PixelFormat_R32F = 0xB,
	PixelFormat_RGBA32F = 0xC,
	PixelFormat_RGBA16F = 0xD,
	PixelFormat_RG1616 = 0xE,
	PixelFormat_RG1616F = 0xF,
	PixelFormat_BC6 = 0x10,
	PixelFormat_BC7 = 0x11,
	AutoSelect = 0x12,
	NbPixelFormats = 0x13,
};

enum TextureFormat
{
	Tex1D = 0x0,
	Tex2D = 0x1,
	TexCubeMap = 0x2,
	Tex3D = 0x3,
	NbTextureFormats = 0x4,
};

enum GammaSettings
{
	Gamma_Linear = 0x0,
	Gamma_sRGB = 0x1,
};

enum AddressMode
{
	AddressMode_Wrap = 0x0,
	AddressMode_Mirror = 0x1,
	AddressMode_Clamp = 0x2,
	AddressMode_Border = 0x3,
	AddressMode_MirrorOnce = 0x4,
	NbAddressModes = 0x5,
};

enum FilterType
{
	Filter_Point = 0x0,
	Filter_Linear = 0x1,
	Filter_None = 0x2,
	Filter_Aniso = 0x3,
	Filter_ComparisonLinear = 0x4,
	NbFilterTypes = 0x5,
};

enum TextureMapType
{
	MapDiffuse = 0x0,
	MapNormal = 0x1,
	MapSpecular = 0x2,
	MapSpecularPower = 0x3,
	MapOffsetBump = 0x4,
	MapEmissive = 0x5,
	MapTransmission = 0x6,
	MapOcclusion = 0x7,
	MapMask1 = 0x8,
	MapMask2 = 0x9,
	MapGeneric = 0xA,
	MapMaxNbTypes = 0xB,
};

enum UserResourceCategory
{
	_Unknown = 0x0,
	CHR_Misc = 0x1,
	CHR_Crowd = 0x2,
	CHR_Unique = 0x3,
	CHR_Heads = 0x4,
	CHR_Weapon = 0x5,
	CIN_Misc = 0x6,
	ENV_Exotic = 0x7,
	DBG_Misc = 0x8,
	ENV_Misc = 0x9,
	ENV_Building = 0xA,
	ENV_Landmark = 0xB,
	ENV_Prop = 0xC,
	ENV_Ground = 0xD,
	ENV_Water = 0xE,
	ENV_Sky = 0xF,
	ENV_Matte = 0x10,
	ENV_Vegetation = 0x11,
	ENV_Fake = 0x12,
	ENV_Lighting = 0x13,
	GFX_Misc = 0x14,
	GFX_Animus = 0x15,
	ENV_LGS = 0x16,
	MISC_Collectibles = 0x17,
	MISC_Illegal = 0x18,
	MIS_Misc = 0x19,
	UI_Misc = 0x1A,
	CHR_Shared = 0x1B,
	CHR_Allies = 0x1C,
	CHR_Enemies = 0x1D,
	CHR_Gameplay = 0x1E,
	ENV_Clutter = 0x1F,
	ENV_Snow = 0x20,
	CHR_Animals = 0x21,
	CHR_Playable = 0x22,
	CHR_Props = 0x23,
	CHR_CinProps = 0x24,
	ANM_Facial = 0x25,
	ANM_Cinematics = 0x26,
	ANM_Cinematics_Streaming = 0x27,
	ANM_Scripted_Events = 0x28,
	ANM_Animals = 0x29,
	ANM_Animal_Horse = 0x2A,
	ANM_Objects = 0x2B,
	ANM_World_Specific = 0x2C,
	ANM_CS_Char_Specific = 0x2D,
	ANM_CS_Gameplay = 0x2E,
	ANM_CS_Crowd = 0x2F,
	ANM_CS_Unique = 0x30,
	ANM_CS_CrowdCAS = 0x31,
	ANM_CS_CrowdLife = 0x32,
	ANM_CG_Char_Generic = 0x33,
	ANM_CG_Navigation = 0x34,
	ANM_CG_Nav_Climb = 0x35,
	ANM_CG_Nav_Climb_AC3 = 0x36,
	ANM_CG_Nav_Ground = 0x37,
	ANM_CG_Nav_Tree = 0x38,
	ANM_CG_Nav_Horse = 0x39,
	ANM_CG_Nav_InAir = 0x3A,
	ANM_CG_Fight = 0x3B,
	ANM_CG_Fight_AC3 = 0x3C,
	ANM_CG_Behavior = 0x3D,
	ANM_Camera = 0x3E,
	ANM_GFX = 0x3F,
	MISC_IgnoreRegionLayoutMeshShape = 0x40,
	SND_RegionLayout = 0x41,
	ENV_Decals = 0x42,
	DEST_RealBlast = 0x43,
	NG_NextGen = 0x44,
	Vehicle_Gameplay = 0x45,
	Vehicle_Shared = 0x46,
	Vehicle_Unique = 0x47,
	GFX_Gen = 0x48,
	GEN_Nav = 0x49,
	Total_NumUserCategory = 0x4A,
};

enum eMirrorAxis
{
	Mirror_AxisNone,
	Mirror_AxisX,
	Mirror_AxisY,
	Mirror_AxisZ,
	Mirror_AxisCount,
};
