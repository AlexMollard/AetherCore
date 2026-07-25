// 2D shadow occluder mask. Two forms of caster, both rasterised into the same R8 coverage mask that
// $Light2DComposite ray-marches (and derives surface normals from):
//   * Tiles (quad): draws the cell and samples that tile's OWN texture, writing coverage from the
//     artwork's alpha - occlusion is sub-tile, following the drawn shape rather than the square cell.
//   * Capsules: a segment a->b thickened by a radius, for script-submitted strokes (drawn ink, trails).
#include "include/DevicePointer.slangh"
#include "include/FrameConstants.slangh"

[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

static const uint kFlipX = 1u;
static const uint kFlipY = 2u;
static const uint kCapsule = 4u;

// 48 bytes. Mirrors aether::Occluder2D in RenderFramePacket.hpp.
struct GpuOccluder2D
{
    float4 posHalfSize; // quad: xy = centre, z = half cell | capsule: xy = a, z = radius
    float4 uvRect;      // quad: atlas region (begin.xy, end.zw) | capsule: xy = b
    uint   textureIndex;
    uint   flags;
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
    float2 uv       : TEXCOORD0; // quad: atlas uv | capsule: world xy
    nointerpolation uint textureIndex : TEXCOORD1;
    nointerpolation uint flags        : TEXCOORD2;
    nointerpolation float4 capsule    : TEXCOORD3; // capsule: (a.xy, b.xy)
    nointerpolation float radius      : TEXCOORD4;
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
    FrameConstantsData* frame = pc.frameConstants.Ptr();

    VSOutput o;
    o.textureIndex = occ.textureIndex;
    o.flags = occ.flags;
    o.capsule = float4(occ.posHalfSize.xy, occ.uvRect.xy);
    o.radius = occ.posHalfSize.z;

    if ((occ.flags & kCapsule) != 0u)
    {
        // Cover the segment's AABB expanded by the radius; the fragment carves the capsule via SDF.
        const float2 a = occ.posHalfSize.xy;
        const float2 b = occ.uvRect.xy;
        const float r = occ.posHalfSize.z;
        const float2 mn = min(a, b) - r;
        const float2 mx = max(a, b) + r;
        const float2 world = lerp(mn, mx, c);
        o.position = mul(frame->viewProj, float4(world, 0.0, 1.0));
        o.uv = world; // world position, for the SDF
        return o;
    }

    const float2 world = occ.posHalfSize.xy + (c * 2.0 - 1.0) * occ.posHalfSize.z;
    if ((occ.flags & kFlipX) != 0u) { c.x = 1.0 - c.x; }
    if ((occ.flags & kFlipY) != 0u) { c.y = 1.0 - c.y; }
    o.position = mul(frame->viewProj, float4(world, 0.0, 1.0));
    // uvRect is image space (v down); the quad is +y up, so flip v like the sprite shader does.
    o.uv = lerp(occ.uvRect.xy, occ.uvRect.zw, float2(c.x, 1.0 - c.y));
    return o;
}

// Distance from p to segment a-b.
float sdSegment(float2 p, float2 a, float2 b)
{
    const float2 pa = p - a;
    const float2 ba = b - a;
    const float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-6), 0.0, 1.0);
    return length(pa - ba * h);
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    if ((input.flags & kCapsule) != 0u)
    {
        const float d = sdSegment(input.uv, input.capsule.xy, input.capsule.zw);
        if (d > input.radius)
        {
            discard;
        }
        return float4(1.0, 0.0, 0.0, 0.0);
    }

    // Coverage = the tile art's alpha. Transparent texels let light through, so shadows match the
    // silhouette the player actually sees.
    const float a = g_textures[input.textureIndex].SampleLevel(g_linearSampler, input.uv, 0.0).a;
    if (a < 0.35)
    {
        discard;
    }
    return float4(a, 0.0, 0.0, 0.0); // R8 mask
}
