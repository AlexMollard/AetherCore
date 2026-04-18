// voxel_shadow_depth.slang - depth-only shadow caster for voxel chunk geometry.
//
// Vertex ABI (matches aether::VoxelVertex, 24 bytes):
//   loc 0  POSITION    float3   — world-space vertex position
//   loc 1  PACKED      uint     — unused in shadow pass
//   loc 2  TEXCOORD_0  float2   — unused in shadow pass
//
// Voxel vertices are already in world space; no skinning required.

#include "include/FrameConstants.slangh"
#include "include/RenderContracts.slangh"

[[vk::push_constant]] DrawPushConstants pc;

struct VSInput
{
    [[vk::location(0)]] float3 position : POSITION;
};

struct VSOutput
{
    float4 svPosition : SV_Position;
};

[shader("vertex")]
VSOutput vertexMain(VSInput input, uint instanceId : SV_InstanceID, uint startInstance : SV_StartInstanceLocation)
{
    FrameConstantsData* fc        = (FrameConstantsData*)(pc.frameAddr);
    DrawInstanceData*   instances = (DrawInstanceData*)(pc.instanceDataAddr);
    const uint instanceIndex = startInstance + instanceId;
    const float4x4 model = instances[instanceIndex].model;

    // Voxel vertices are emitted in world space; model is identity but kept
    // for correctness if that ever changes.
    const float4 worldPos = mul(model, float4(input.position, 1.0));

    VSOutput o;
    o.svPosition = mul(fc->viewProj, worldPos);
    return o;
}

[shader("fragment")]
void fragmentMain()
{
}
