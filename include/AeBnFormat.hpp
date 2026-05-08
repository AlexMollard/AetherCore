#pragma once

#include <cstdint>

// AEBN v2 - Full mesh + animation binary format.
// Produced by AssetPacker MeshProcessor; consumed by GltfAsset::LoadFromVfsPath.
//
// File layout (all multi-byte integers are little-endian):
//
//   AeBnHeader                            (fixed, 36 bytes)
//   imageCount × { AeBnImageHeader, name bytes, uri bytes }
//   textureCount × { AeBnTextureHeader, name bytes }
//   materialCount × { AeBnMaterialHeader, name bytes }
//   nodeCount × { AeBnNodeHeader, uint32_t children[childCount], name bytes }
//   skinCount × { AeBnSkinHeader, name bytes, uint32_t joints[jointCount],
//                 float ibms[jointCount × 16] }
//   primitiveCount × { AeBnPrimitiveHeader, AeBnVertex[vertexCount],
//                      uint32_t[indexCount] }
//   animCount × { AeBnAnimHeader, name bytes,
//                 channelCount × { AeBnChannelHeader,
//                                  float times[keyCount],
//                                  float values[keyCount × 4] } }
//
// Image URIs are stored as relative paths with the image extension replaced by
// ".texture" (e.g. "textures/fox_diffuse.texture").  The engine resolves them
// against the .mesh file's VFS directory the same way GLTF resolves .bin URIs.

inline constexpr char     AEBN_MAGIC[4] = { 'A', 'E', 'B', 'N' };
inline constexpr uint32_t AEBN_VERSION  = 1;

enum class AeBnAnimPath : uint8_t
{
    Translation = 0,
    Rotation    = 1,
    Scale       = 2,
    Weights     = 3,
};

enum class AeBnInterp : uint8_t
{
    Linear      = 0,
    Step        = 1,
    CubicSpline = 2,
};

#pragma pack(push, 1)

struct AeBnHeader
{
    char     magic[4]       = { 'A', 'E', 'B', 'N' };
    uint32_t version        = AEBN_VERSION;
    uint32_t imageCount     = 0;
    uint32_t textureCount   = 0;
    uint32_t materialCount  = 0;
    uint32_t nodeCount      = 0;
    uint32_t skinCount      = 0;
    uint32_t primitiveCount = 0;
    uint32_t animCount      = 0;
    // 36 bytes total
};

// Followed by: char name[nameLen], char uri[uriLen]
struct AeBnImageHeader
{
    uint16_t nameLen;
    uint16_t uriLen;
};

// Followed by: char name[nameLen]
struct AeBnTextureHeader
{
    int32_t  imageIndex; // -1 = none
    uint16_t nameLen;
};

// Followed by: char name[nameLen]
struct AeBnMaterialHeader
{
    float    baseColorFactor[4];
    float    metallicFactor;
    float    roughnessFactor;
    float    emissiveFactor[3];
    float    alphaCutoff;
    int32_t  baseColorTexture;          // -1 = none
    int32_t  metallicRoughnessTexture;  // -1 = none
    int32_t  normalTexture;             // -1 = none
    int32_t  occlusionTexture;          // -1 = none
    int32_t  emissiveTexture;           // -1 = none
    uint8_t  doubleSided;
    uint8_t  alphaBlend;
    uint8_t  alphaMask;
    uint8_t  _pad;
    uint16_t nameLen;
};

// Followed by: uint32_t children[childCount], char name[nameLen]
struct AeBnNodeHeader
{
    int32_t  parentIndex; // -1 = root
    int32_t  meshIndex;   // -1 = no mesh
    int32_t  skinIndex;   // -1 = no skin
    float    translation[3];
    float    rotation[4]; // stored xyzw
    float    scale[3];
    float    matrix[16];  // column-major; valid only when hasMatrix != 0
    uint8_t  hasMatrix;
    uint8_t  _pad[3];
    uint32_t childCount;
    uint16_t nameLen;
};

// Followed by: char name[nameLen], uint32_t joints[jointCount],
//              float ibms[jointCount × 16]  (column-major)
struct AeBnSkinHeader
{
    int32_t  skeletonRoot; // -1 = none
    uint32_t jointCount;
    uint16_t nameLen;
};

// Followed by: AeBnVertex[vertexCount], uint32_t[indexCount]
struct AeBnPrimitiveHeader
{
    uint32_t nodeIndex;
    int32_t  materialIndex; // -1 = no material
    int32_t  skinIndex;     // -1 = no skin
    uint32_t vertexCount;
    uint32_t indexCount;
};

// Interleaved vertex matching Mesh::Vertex field order (92 bytes).
struct AeBnVertex
{
    float    position[3];
    float    normal[3];
    float    tangent[4];
    float    uv[2];
    float    color[3];
    uint32_t jointIndices[4];
    float    jointWeights[4];
};

static_assert(sizeof(AeBnVertex) == 92);

// Followed by: float times[keyCount], float values[keyCount × 4]
// values are vec4: Translation/Scale have w=0, Rotation is xyzw quaternion.
struct AeBnChannelHeader
{
    uint32_t nodeIndex;
    uint8_t  path;    // AeBnAnimPath
    uint8_t  interp;  // AeBnInterp
    uint8_t  _pad[2];
    uint32_t keyCount;
};

// Followed by: char name[nameLen],
//              channelCount × { AeBnChannelHeader, times, values }
struct AeBnAnimHeader
{
    uint32_t channelCount;
    uint16_t nameLen;
};

#pragma pack(pop)

static_assert(sizeof(AeBnHeader)          == 36);
static_assert(sizeof(AeBnImageHeader)     == 4);
static_assert(sizeof(AeBnTextureHeader)   == 6);
static_assert(sizeof(AeBnMaterialHeader)  == 66);  // 16+4+4+12+4+5×4+4×1+2
static_assert(sizeof(AeBnNodeHeader)      == 126); // 3×4+12+16+12+64+4+4+2
static_assert(sizeof(AeBnSkinHeader)      == 10);
static_assert(sizeof(AeBnPrimitiveHeader) == 20);
static_assert(sizeof(AeBnChannelHeader)   == 12);
static_assert(sizeof(AeBnAnimHeader)      == 6);
