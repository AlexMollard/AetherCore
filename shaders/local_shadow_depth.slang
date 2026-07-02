// VSM shadow caster for local lights.
// Outputs float2(depth, depth*depth) to color attachment, where depth is the
// LINEAR radial distance from the light normalized by the light range.
// Raw NDC z is useless for VSM here: perspective compresses everything past a
// few meters into ~[0.99, 1.0], so occluder/receiver deltas drown under the
// variance floor and distant shadows wash out entirely.
// Vertex data fetched via BDA (DrawInstanceData.vertexBufferAddr).
// Light VP matrix comes from per-light FrameConstants via BDA in push constants.
// The per-light FrameConstants repurpose cameraWorldPos as xyz=light position,
// w=light range (see LocalShadowService::BuildFrameShadowData).

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
    FrameConstantsData* fc = pc.frame.Ptr();
    const float3 lightPos = fc->cameraWorldPos.xyz;
    const float range = max(fc->cameraWorldPos.w, 1e-3);
    const float depth = saturate(distance(input.worldPos, lightPos) / range);
    return float2(depth, depth * depth);
}
