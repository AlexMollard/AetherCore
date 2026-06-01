// Forward shader for GPU-driven voxel chunk geometry.
//
// Vertex ABI (matches aether::VoxelVertex, 24 bytes):
//   loc 0  POSITION    float3   - world-space vertex position
//   loc 1  PACKED      uint     - bits[0-2]=faceIndex(0-5), bits[3-4]=aoLevel(0-3)
//   loc 2  TEXCOORD_0  float2   - atlas UV coordinates
//
// Push constants (24 bytes, matches aether::DrawPushConstants):
//   offset  0 : uint64_t  frameAddr         BDA -> FrameConstantsData
//   offset  8 : uint64_t  instanceDataAddr  BDA -> DrawInstanceData[]
//   offset 16 : uint64_t  skinPaletteAddr   (unused for voxel draws)
//
// Material convention:
//   DrawInstanceData.materialIndex -> GpuMaterial[] (via materialBufferAddr BDA)
//   GpuMaterial.albedoSlot         -> bindless atlas texture
//
// Lighting model: Lambertian diffuse + ambient + directional sun.
// AO baked into vertex packed field.  No PBR (appropriate for stylized voxel look).

[[vk::binding(0, 0)]] Texture2D    g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;
[[vk::binding(2, 0)]] SamplerState g_voxelAtlasSampler;

#include "include/FrameConstants.slangh"
#include "include/RenderContracts.slangh"
#include "include/VoxelVertex.slangh"
#include "include/GpuMaterial.slangh"
#include "include/DirectionalShadowCSM.slangh"

static const float   kAOLevels[4] = { 1.0f, 0.75f, 0.55f, 0.35f };

// Face normals indexed by faceIndex (0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z).
static const float3 kFaceNormals[6] = {
    float3( 1,  0,  0),
    float3(-1,  0,  0),
    float3( 0,  1,  0),
    float3( 0, -1,  0),
    float3( 0,  0,  1),
    float3( 0,  0, -1),
};

[[vk::push_constant]] DrawPushConstants pc;

struct VSOutput
{
    float4                              svPosition   : SV_Position;
    [[vk::location(0)]] float3          worldNormal  : TEXCOORD0;
    [[vk::location(1)]] float2          uv           : TEXCOORD1;
    [[vk::location(2)]] float           ao           : TEXCOORD2;
    [[vk::location(3)]] nointerpolation uint materialIndex : TEXCOORD3;
    [[vk::location(4)]] float3          worldPos     : TEXCOORD4;
};

[shader("vertex")]
VSOutput vertexMain(uint vertexId : SV_VertexID, uint startInstance : SV_StartInstanceLocation)
{
    FrameConstantsData* fc        = (FrameConstantsData*)(pc.frameAddr);
    DrawInstanceData*   instances = (DrawInstanceData*)(pc.instanceDataAddr);
    const uint instanceIndex = startInstance;

    VoxelVertex* verts = (VoxelVertex*)(instances[instanceIndex].vertexBufferAddr);
    VoxelVertex input = verts[vertexId];

    const float4x4 model    = instances[instanceIndex].model;
    const float4   worldPos = mul(model, float4(input.position, 1.0));

    const uint faceIndex = input.packed & 0x7u;
    const uint aoLevel   = (input.packed >> 3) & 0x3u;

    VSOutput o;
    o.svPosition   = mul(fc->viewProj, worldPos);
    o.worldNormal  = kFaceNormals[min(faceIndex, 5u)];
    o.uv           = input.uv;
    o.ao           = kAOLevels[aoLevel];
    o.materialIndex = instances[instanceIndex].materialIndex;
    o.worldPos     = worldPos.xyz;
    return o;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    FrameConstantsData* fc = (FrameConstantsData*)(pc.frameAddr);

    // Resolve atlas texture from the material.
    float4 baseColor = float4(0.8f, 0.5f, 0.3f, 1.0f); // pink fallback
    if (input.materialIndex != kNoTexture)
    {
        GpuMaterial* mat = (GpuMaterial*)(fc->materialBufferAddr) + input.materialIndex;
        if (mat->albedoSlot != kNoTexture)
            baseColor = g_textures[mat->albedoSlot].SampleLevel(g_voxelAtlasSampler, input.uv, 0.0f);
    }

    // Directional diffuse.
    const float3 sunDir  = normalize(fc->sunDirectionIntensity.xyz);
    const float  NdotL   = max(dot(input.worldNormal, sunDir), 0.0f);
    const float shadow = ComputeDirectionalShadow(fc, input.worldPos, input.worldNormal, sunDir);
    const float sunVisibility = lerp(1.0, shadow, fc->shadowParams.z);
    const float3 diffuse = baseColor.rgb * (fc->sunColor.rgb * NdotL * fc->sunDirectionIntensity.w * sunVisibility);

    // Ambient.
    const float3 ambient = baseColor.rgb * fc->ambientColor.rgb;

    // Apply vertex AO.
    const float3 finalColor = (diffuse + ambient) * input.ao;

    return float4(finalColor, baseColor.a);
}
