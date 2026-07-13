#pragma once

#include <cstdint>

// ============================================================================
// AAA Asset Pipeline - Disk Format Definitions
//
// All structs below describe ON-DISK layout only. They use #pragma pack(1)
// and must NEVER be directly cast from raw file memory. Runtime loading uses
// BinaryReader (src/engine/utils/BinaryReader.hpp) which memcpy's into
// properly-aligned local variables.
//
// All multi-byte integers are little-endian.
// ============================================================================

#pragma pack(push, 1)

// ============================================================================
// .skel - Skeleton Definition
// ============================================================================

inline constexpr char SKEL_MAGIC[4] = {'S', 'K', 'E', 'L'};
inline constexpr uint32_t SKEL_VERSION = 1;

struct SkelHeaderDisk
{
	char magic[4] = {'S', 'K', 'E', 'L'};
	uint32_t version = SKEL_VERSION;
	uint32_t boneCount = 0;
	uint16_t nameLen = 0;
	uint64_t skeletonHash = 0; // XXH3-64 on sorted bone payload
	// Total: 4+4+4+2+8 = 22 bytes
};

struct BoneEntryHeaderDisk
{
	uint16_t nameLen = 0;
	// Followed by: name[nameLen], parentIndex(int32), ibm[16](float, column-major)
};

// ============================================================================
// .mesh - Geometry Only
// ============================================================================

inline constexpr char MESH_MAGIC[4] = {'M', 'E', 'S', 'H'};
inline constexpr uint32_t MESH_VERSION = 3;

struct MeshHeaderDisk
{
	char magic[4] = {'M', 'E', 'S', 'H'};
	uint32_t version = MESH_VERSION;
	uint32_t vertexCount = 0;
	uint32_t indexCount = 0;
	uint32_t skinRefPathLen = 0; // 0 = no skin
	uint32_t materialCount = 0;
	uint32_t subMeshCount = 0; // number of SubMeshHeaderDisk entries (v3+)
	float aabbMin[3] = {0, 0, 0};
	float aabbMax[3] = {0, 0, 0};
	float sphereCenter[3] = {0, 0, 0};
	float sphereRadius = 0;
	uint8_t indexType = 0; // 0 = uint16, 1 = uint32
	uint8_t _pad[3] = {0, 0, 0};
	// Total: 4+4+4+4+4+4+12+12+12+4+1+3 = 72 bytes
};

struct SubMeshHeaderDisk
{
	uint32_t firstIndex;    // index into the merged index buffer
	uint32_t indexCount;    // number of indices in this submesh
	uint32_t materialIndex; // index into the material paths array
	// Total: 12 bytes
};

// Disk vertex - 96 bytes, aligned to 16/32-byte cache lines.
// Converted to aether::Mesh::Vertex (100 bytes) at load time via
// GltfAsset::LoadFromMesh. Note: disk stores color as packed uint32 RGBA8
// while runtime stores it as unpacked vec3 RGB.
struct DiskMeshVertex
{
	float position[3];        // 12 bytes
	float normal[3];          // 12 bytes
	float tangent[4];         // 16 bytes
	float uv[2];              // 8 bytes
	uint32_t color;           // 4 bytes - packed RGBA8
	float uv2[2];             // 8 bytes - secondary UV
	uint8_t _pad[4];          // 4 bytes - explicit alignment padding
	uint32_t jointIndices[4]; // 16 bytes - 0xFFFFFFFF = unused slot
	float jointWeights[4];    // 16 bytes
	// Total: 12+12+16+8+4+8+4+16+16 = 96 bytes
};

static_assert(sizeof(DiskMeshVertex) == 96);

// ============================================================================
// .anim - Single Animation Clip
// ============================================================================

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
	uint16_t flags = 0; // v2+: bit 0 = HasBoneNames
	// v1 layout: 4+4+4+2 = 14 bytes (no flags field)
	// v2 layout: 4+4+4+2+2 = 16 bytes
};

struct ChannelHeaderDisk
{
	uint32_t nodeIndex = 0;
	uint8_t path = 0;   // AnimPathDisk
	uint8_t interp = 0; // AnimInterpDisk
	uint8_t _pad[2] = {0, 0};
	uint32_t keyCount = 0;
	// Total: 4+1+1+2+4 = 12 bytes
};

// ============================================================================
// .animset - Animation Set Bundle
// ============================================================================

inline constexpr char ASET_MAGIC[4] = {'A', 'S', 'E', 'T'};
inline constexpr uint32_t ASET_VERSION = 1;

struct AnimSetHeaderDisk
{
	char magic[4] = {'A', 'S', 'E', 'T'};
	uint32_t version = ASET_VERSION;
	uint32_t animCount = 0;
	uint64_t skeletonHash = 0; // XXH3-64 from .skel
	uint32_t _pad = 0;         // explicit padding to 24
	// Total: 4+4+4+8+4 = 24 bytes
};

// ============================================================================
// .material - Material Preset
// ============================================================================

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
	float baseColorFactor[4] = {1, 1, 1, 1}; // 16 bytes
	float metallicFactor = 1;                // 4 bytes
	float roughnessFactor = 1;               // 4 bytes
	float emissiveFactor[3] = {0, 0, 0};     // 12 bytes
	float alphaCutoff = 0;                   // 4 bytes
	uint8_t doubleSided = 0;                 // 1 byte
	uint8_t alphaBlend = 0;                  // 1 byte
	uint8_t alphaMask = 0;                   // 1 byte
	uint8_t texturePathCount = 0;            // 1 byte
	uint8_t _pad[12] = {0};                  // 12 bytes → 64 total
	// Total: 4+4+16+4+4+12+4+1+1+1+1+12 = 64 bytes
};

// Trailing sections after MaterialHeaderDisk (not reflected in MATL_VERSION,
// which only governs the fixed header layout above):
//   texturePathCount * { uint8_t type; uint16_t pathLen; char path[pathLen]; }
//   uint16_t shaderPathLen; char shaderVfsPath[shaderPathLen];   (Phase 5)
// The trailing shader-path string is a length-prefixed string in the same
// format BinaryReader::ReadString() produces/consumes. It is ALWAYS written
// (length 0 = "no per-material shader override, use the template default").
// A pre-Phase-5 blob simply has no bytes here; BinaryReader::ReadString()
// on an exhausted buffer returns "" (CanRead fails -> Read<uint16_t>() is 0),
// which reads identically to an explicit empty override, so this is backward-
// compatible without a MATL_VERSION bump.

#pragma pack(pop)

static_assert(sizeof(SkelHeaderDisk) == 22);
static_assert(sizeof(BoneEntryHeaderDisk) == 2);
static_assert(sizeof(MeshHeaderDisk) == 72);
static_assert(sizeof(SubMeshHeaderDisk) == 12);
static_assert(sizeof(AnimHeaderDisk) == 16);
static_assert(sizeof(ChannelHeaderDisk) == 12);
static_assert(sizeof(AnimSetHeaderDisk) == 24);
static_assert(sizeof(MaterialHeaderDisk) == 64);

// ── Magic + version validation helpers ───────────────────────────────────

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
