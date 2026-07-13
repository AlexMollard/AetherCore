// EVSM shadow caster for local lights.
// Outputs float2(exp(c*d), exp(c*d)^2) moments, where d is the LINEAR radial
// distance from the light normalized by the light range.
// Linear (not NDC) depth: perspective z compresses everything past a few
// meters into ~[0.99, 1.0], so occluder/receiver deltas drown under the
// variance floor and distant shadows wash out entirely.
// Exponential warp: plain VSM bleeds light where two occluders at different
// depths overlap (bright outlines inside shadows); the warp makes Chebyshev's
// bound effectively exact there.
// Vertex data fetched via BDA (DrawInstanceData.vertexBufferAddr).
// Light VP matrix comes from per-light FrameConstants via BDA in push constants.
// The per-light FrameConstants repurpose cameraWorldPos as xyz=light position,
// w=light range (see LocalShadowService::BuildFrameShadowData).

// Must match kEvsmExponent in LocalShadow.slangh and LocalShadowService.cpp
// (atlas clear color = exp(c), exp(2c)).
static const float kEvsmExponent = 40.0;

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
    const float warped = exp(kEvsmExponent * depth);
    return float2(warped, warped * warped);
}
