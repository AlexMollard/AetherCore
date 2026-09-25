// depth-only shadow caster for directional light.
// Vertex data fetched via buffer device address (DrawInstanceData.vertexBufferAddr).

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
DepthVSOutput vertexMain(uint vertexId : SV_VertexID, uint startInstance : SV_StartInstanceLocation)
{
    float3 worldPos;
    float2 uv;
    uint materialIndex;
    float4 clipPos = DepthVertexTransform(vertexId, startInstance, worldPos, uv, materialIndex);

    DepthVSOutput o;
    o.svPosition = clipPos;
    o.uv = uv;
    o.materialIndex = materialIndex;
    return o;
}

[shader("fragment")]
void fragmentMain(DepthVSOutput input)
{
    // Foliage/cutout cards DO cast, cut out by the alpha test below: the original's umbrella
    // tops and palm leaves shade the ground. With prelit receivers the old clump-blotch
    // problem is gone - a shadow can only pull ground to the bake's own shade, never darker.
    //
    // Alpha-BLENDED surfaces never cast: the sea is one huge double-sided plane over the
    // water, and the sky dome wraps the whole level - either of them casting paints a hard
    // band (or the entire beach) with the sun's own shadow. ponytail: a genuinely
    // shadow-casting transparent surface would need a per-material opt-in instead.
    if (input.materialIndex != kNoTexture)
    {
        FrameConstantsData* fc = pc.frame.Ptr();
        const GpuMaterial mat = ((GpuMaterial*)(fc->materialBufferAddr))[input.materialIndex];
        if ((mat.flags & kFlagAlphaBlend) != 0u)
        {
            discard;
        }
    }
    AlphaMaskDiscard(input.materialIndex, input.uv);
}
