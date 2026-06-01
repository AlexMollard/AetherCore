// VSM shadow caster for local lights.
// Outputs float2(depth, depth*depth) to color attachment.
// Vertex data fetched via BDA (DrawInstanceData.vertexBufferAddr).
// Light VP matrix comes from per-light FrameConstants via BDA in push constants

#include "include/FrameConstants.slangh"
#include "include/RenderContracts.slangh"
#include "include/MeshVertex.slangh"

[[vk::push_constant]] DrawPushConstants pc;

#include "include/DepthVertex.slangh"

[shader("vertex")]
DepthVSOutputWithWorldPos vertexMain(uint vertexId : SV_VertexID, uint startInstance : SV_StartInstanceLocation)
{
    float3 worldPos;
    float4 clipPos = DepthVertexTransform(vertexId, startInstance, worldPos);

    DepthVSOutputWithWorldPos o;
    o.svPosition = clipPos;
    o.worldPos   = worldPos;
    return o;
}

[shader("fragment")]
float2 fragmentMain(DepthVSOutputWithWorldPos input) : SV_Target
{
    float depth = input.svPosition.z;
    return float2(depth, depth * depth);
}
