#pragma once

#include <cstdint>

// All structs below describe ON-DISK layout only. They use #pragma pack(1)

#pragma pack(push, 1)

inline constexpr char SKEL_MAGIC[4] = {'S', 'K', 'E', 'L'};
inline constexpr uint32_t SKEL_VERSION = 1;

struct SkelHeaderDisk
{
	char magic[4] = {'S', 'K', 'E', 'L'};
	uint32_t version = SKEL_VERSION;
	uint32_t boneCount = 0;
	uint16_t nameLen = 0;
	uint64_t skeletonHash = 0;
};

struct BoneEntryHeaderDisk
{
	uint16_t nameLen = 0;
};

inline constexpr char MESH_MAGIC[4] = {'M', 'E', 'S', 'H'};
inline constexpr uint32_t MESH_VERSION = 3;

struct MeshHeaderDisk
{
	char magic[4] = {'M', 'E', 'S', 'H'};
	uint32_t version = MESH_VERSION;
	uint32_t vertexCount = 0;
	uint32_t indexCount = 0;
	uint32_t skinRefPathLen = 0;
	uint32_t materialCount = 0;
	uint32_t subMeshCount = 0;
	float aabbMin[3] = {0, 0, 0};
	float aabbMax[3] = {0, 0, 0};
	float sphereCenter[3] = {0, 0, 0};
	float sphereRadius = 0;
	uint8_t indexType = 0;
	uint8_t _pad[3] = {0, 0, 0};
};

struct SubMeshHeaderDisk
{
	uint32_t firstIndex;
	uint32_t indexCount;
	uint32_t materialIndex;
};

struct DiskMeshVertex
{
	float position[3];
	float normal[3];
	float tangent[4];
	float uv[2];
	uint32_t color;
	float uv2[2];
	uint8_t _pad[4]; // 4 bytes - explicit alignment padding
	uint32_t jointIndices[4];
	float jointWeights[4];
};

static_assert(sizeof(DiskMeshVertex) == 96);

inline constexpr char ANIM_MAGIC[4] = {'A', 'N', 'I', 'M'};
inline constexpr uint32_t ANIM_VERSION = 3;

inline constexpr uint16_t ANIM_FLAG_HAS_BONE_NAMES = 1;

enum class AnimPathDisk : uint8_t
{
	Translation = 0,
	Rotation = 1,
	Scale = 2,
	Weights = 3,
};

enum class AnimInterpDisk : uint8_t
{
	Linear = 0,
	Step = 1,
	CubicSpline = 2,
};

struct AnimHeaderDisk
{
	char magic[4] = {'A', 'N', 'I', 'M'};
	uint32_t version = ANIM_VERSION;
	uint32_t channelCount = 0;
	uint16_t nameLen = 0;
	uint16_t flags = 0;
	// v1 layout: 4+4+4+2 = 14 bytes (no flags field)
};

struct ChannelHeaderDisk
{
	uint32_t nodeIndex = 0;
	uint8_t path = 0;
	uint8_t interp = 0;
	uint8_t _pad[2] = {0, 0};
	uint32_t keyCount = 0;
};

inline constexpr char ASET_MAGIC[4] = {'A', 'S', 'E', 'T'};
inline constexpr uint32_t ASET_VERSION = 1;

struct AnimSetHeaderDisk
{
	char magic[4] = {'A', 'S', 'E', 'T'};
	uint32_t version = ASET_VERSION;
	uint32_t animCount = 0;
	uint64_t skeletonHash = 0;
	uint32_t _pad = 0;
};

inline constexpr char MATL_MAGIC[4] = {'M', 'A', 'T', 'L'};
inline constexpr uint32_t MATL_VERSION = 1;

enum class TextureTypeDisk : uint8_t
{
	BaseColor = 0,
	MetallicRoughness = 1,
	Normal = 2,
	Occlusion = 3,
	Emissive = 4,
};

struct MaterialHeaderDisk
{
	char magic[4] = {'M', 'A', 'T', 'L'};
	uint32_t version = MATL_VERSION;
	float baseColorFactor[4] = {1, 1, 1, 1};
	float metallicFactor = 1;
	float roughnessFactor = 1;
	float emissiveFactor[3] = {0, 0, 0};
	float alphaCutoff = 0;
	uint8_t doubleSided = 0;
	uint8_t alphaBlend = 0;
	uint8_t alphaMask = 0;
	uint8_t texturePathCount = 0;
	// Multiply base colour by the mesh's COLOR_0 (glTF's rule). Carved from the old padding, so
	// files written before it read 0 - exactly how they rendered.
	uint8_t modulateVertexColor = 0;
	// PS2 foliage/cutout card (tw-extract marks alpha-masked scenery "foliage"): shaded by its
	// baked vertex colour with a wrapped diffuse and excluded from shadow casting/receiving.
	// Carved from the same padding - files written before it read 0, i.e. ordinary geometry.
	uint8_t foliage = 0;
	uint8_t _pad0[2] = {0, 0};
	// UV scroll velocity in UV units per second. Carved from the old padding like
	// modulateVertexColor: files written before it read {0, 0} - a static texture, which
	// is exactly how they rendered. Old .material files therefore stay valid unchanged.
	float uvScroll[2] = {0, 0};
};

// which only governs the fixed header layout above):

#pragma pack(pop)

static_assert(sizeof(SkelHeaderDisk) == 22);
static_assert(sizeof(BoneEntryHeaderDisk) == 2);
static_assert(sizeof(MeshHeaderDisk) == 72);
static_assert(sizeof(SubMeshHeaderDisk) == 12);
static_assert(sizeof(AnimHeaderDisk) == 16);
static_assert(sizeof(ChannelHeaderDisk) == 12);
static_assert(sizeof(AnimSetHeaderDisk) == 24);
static_assert(sizeof(MaterialHeaderDisk) == 64);

inline bool CheckMagic(const SkelHeaderDisk& h)
{
	return h.magic[0] == 'S' && h.magic[1] == 'K' && h.magic[2] == 'E' && h.magic[3] == 'L' && h.version == SKEL_VERSION;
}

inline bool CheckMagic(const MeshHeaderDisk& h)
{
	return h.magic[0] == 'M' && h.magic[1] == 'E' && h.magic[2] == 'S' && h.magic[3] == 'H' && h.version == MESH_VERSION;
}

inline bool CheckMagic(const AnimHeaderDisk& h)
{
	return h.magic[0] == 'A' && h.magic[1] == 'N' && h.magic[2] == 'I' && h.magic[3] == 'M' && (h.version == ANIM_VERSION || h.version == 2 || h.version == 1);
}

inline bool CheckMagic(const AnimSetHeaderDisk& h)
{
	return h.magic[0] == 'A' && h.magic[1] == 'S' && h.magic[2] == 'E' && h.magic[3] == 'T' && h.version == ASET_VERSION;
}

inline bool CheckMagic(const MaterialHeaderDisk& h)
{
	return h.magic[0] == 'M' && h.magic[1] == 'A' && h.magic[2] == 'T' && h.magic[3] == 'L' && h.version == MATL_VERSION;
}
