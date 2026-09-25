// Depth caster for local (point/spot) light shadows.
// Writes LINEAR radial distance from the light, normalized by the light range.
//
// Linear (not NDC) depth: perspective z compresses everything past a few
// metres into ~[0.99, 1.0], which leaves no usable precision for the blocker
// search PCSS runs against this atlas. Radial distance is also face-invariant,
// so a point light's six cube faces share one comparable metric and a receiver
// can be tested against whichever face contains it.
// Vertex data fetched via BDA (DrawInstanceData.vertexBufferAddr).
// Light VP matrix comes from per-light FrameConstants via BDA in push constants.
// The per-light FrameConstants repurpose cameraWorldPos as xyz=light position,
// w=light range (see LocalShadowService::BuildFrameShadowData).

// The bindless heap must be at set 0 - every other set reads garbage on this backend - and
// has to be declared before the headers that sample through it.
[[vk::binding(0, 0)]] Texture2D    g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

#include "include/FrameConstants.slangh"
#include "include/GpuMaterial.slangh"
#include "include/RenderContracts.slangh"
#include "include/MeshVertex.slangh"

[[vk::push_constant]] DrawPushConstants pc;

#include "include/DepthVertex.slangh"
#include "include/AlphaMaskTest.slangh"

[shader("vertex")]
DepthVSOutputWithWorldPos vertexMain(uint vertexId : SV_VertexID, uint startInstance : SV_StartInstanceLocation)
{
    float3 worldPos;
    float2 uv;
    uint materialIndex;
    float4 clipPos = DepthVertexTransform(vertexId, startInstance, worldPos, uv, materialIndex);

    DepthVSOutputWithWorldPos o;
    o.svPosition = clipPos;
    o.worldPos   = worldPos;
    o.uv = uv;
    o.materialIndex = materialIndex;
    return o;
}

[shader("fragment")]
float fragmentMain(DepthVSOutputWithWorldPos input) : SV_Target
{
    AlphaMaskDiscard(input.materialIndex, input.uv);

    // Foliage never casts, matching the directional cascades (see shadow_depth.slang).
    if (input.materialIndex != kNoTexture)
    {
        const GpuMaterial mat = ((GpuMaterial*)(pc.frame.Ptr()->materialBufferAddr))[input.materialIndex];
        if ((mat.flags & kFlagFoliage) != 0u)
        {
            discard;
        }
    }

    FrameConstantsData* fc = pc.frame.Ptr();
    const float3 lightPos = fc->cameraWorldPos.xyz;
    const float range = max(fc->cameraWorldPos.w, 1e-3);
    return saturate(distance(input.worldPos, lightPos) / range);
}
