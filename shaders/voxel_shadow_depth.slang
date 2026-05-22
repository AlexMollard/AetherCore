// voxel_shadow_depth.slang - depth-only shadow caster for voxel chunk geometry.
// Vertex data fetched via buffer device address (DrawInstanceData.vertexBufferAddr).
// Voxel vertices are already in world space; no skinning required.

#include "include/FrameConstants.slangh"
#include "include/RenderContracts.slangh"

[[vk::push_constant]] DrawPushConstants pc;

// Must match aether::VoxelVertex (24 bytes). Only position is used here.
struct VoxelVertex
{
    [[vk::offset(0)]]  float3 position;
    [[vk::offset(12)]] uint   packed;
    [[vk::offset(16)]] float2 uv;
};

struct VSOutput
{
    float4 svPosition : SV_Position;
};

[shader("vertex")]
VSOutput vertexMain(uint vertexId : SV_VertexID, uint startInstance : SV_StartInstanceLocation)
{
    FrameConstantsData* fc        = (FrameConstantsData*)(pc.frameAddr);
    DrawInstanceData*   instances = (DrawInstanceData*)(pc.instanceDataAddr);
    const uint instanceIndex = startInstance;
    const float4x4 model = instances[instanceIndex].model;

    VoxelVertex* verts = (VoxelVertex*)(instances[instanceIndex].vertexBufferAddr);
    const float3 position = verts[vertexId].position;

    // Voxel vertices are emitted in world space; model is identity but kept
    // for correctness if that ever changes.
    const float4 worldPos = mul(model, float4(position, 1.0));

    VSOutput o;
    o.svPosition = mul(fc->viewProj, worldPos);
    return o;
}

[shader("fragment")]
void fragmentMain()
{
}
