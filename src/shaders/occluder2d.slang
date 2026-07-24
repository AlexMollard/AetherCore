// 2D shadow occluder mask. Renders each solid tile cell as a quad and samples that tile's OWN texture,
// writing coverage from the artwork's alpha - so occlusion is sub-tile: shadows follow the drawn shape
// (a sloped rock, a gap between spikes) instead of the whole square cell. The $Light2DComposite pass
// ray-marches this mask and derives surface normals from it.
#include "include/DevicePointer.slangh"
#include "include/FrameConstants.slangh"

[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

// 48 bytes. Mirrors aether::Occluder2D in RenderFramePacket.hpp.
struct GpuOccluder2D
{
    float4 posHalfSize; // xy = world centre, z = half cell size, w unused
    float4 uvRect;      // atlas region, image space (begin.xy, end.zw)
    uint   textureIndex;
    uint   flags;       // bit0 = flipX, bit1 = flipY
    uint   pad0;
    uint   pad1;
};

struct OccluderPush
{
    DevicePtr<FrameConstantsData> frameConstants;
    DevicePtr<GpuOccluder2D> occluders;
    uint   count;
    uint   pad;
};

[[vk::push_constant]] OccluderPush pc;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    nointerpolation uint textureIndex : TEXCOORD1;
};

[shader("vertex")]
VSOutput vertexMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    // Quad corners in 0..1 (y up), matching the sprite pass' convention.
    const float2 corners[6] = {
        float2(0, 0), float2(1, 0), float2(1, 1),
        float2(0, 0), float2(1, 1), float2(0, 1),
    };
    float2 c = corners[vertexId % 6u];

    const GpuOccluder2D occ = pc.occluders.Ptr()[instanceId];
    const float2 world = occ.posHalfSize.xy + (c * 2.0 - 1.0) * occ.posHalfSize.z;

    if ((occ.flags & 1u) != 0u) { c.x = 1.0 - c.x; }
    if ((occ.flags & 2u) != 0u) { c.y = 1.0 - c.y; }

    FrameConstantsData* frame = pc.frameConstants.Ptr();
    VSOutput o;
    o.position = mul(frame->viewProj, float4(world, 0.0, 1.0));
    // uvRect is image space (v down); the quad is +y up, so flip v like the sprite shader does.
    o.uv = lerp(occ.uvRect.xy, occ.uvRect.zw, float2(c.x, 1.0 - c.y));
    o.textureIndex = occ.textureIndex;
    return o;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    // Coverage = the tile art's alpha. Transparent texels let light through, so shadows match the
    // silhouette the player actually sees.
    const float a = g_textures[input.textureIndex].SampleLevel(g_linearSampler, input.uv, 0.0).a;
    if (a < 0.35)
    {
        discard;
    }
    return float4(a, 0.0, 0.0, 0.0); // R8 mask
}
