// 2D screen-space light-map. Runs once, fullscreen, AFTER the sprite/tile layer has been drawn into
// the scene HDR colour and BEFORE tonemap, with MULTIPLY blend (DstColor, Zero) so the hardware does
//   hdrColor.rgb *= light.rgb
// The light map is: ambient (a dark floor) + the radial contribution of every point light and the
// cone contribution of every spot light, each ATTENUATED BY SHADOWS ray-marched through an occluder
// mask (solid tiles). Lit spots brighten/tint the pixel art; walls cast shadow pools behind them.
// Consumes the SAME lights the 3D renderer uses (engine PointLight/SpotLight components).
//
// World reconstruction is exact for a 2D ortho camera: world.xy depends only on screen ndc, not depth.
#include "include/DevicePointer.slangh"
#include "include/FrameConstants.slangh"

[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

// Packed light record (48 bytes). Mirrors aether::GpuLight2D in Light2DCompositor.cpp.
struct GpuLight2D
{
    float4 posRadiusKind;  // xy = world position, z = radius (world units), w = kind (0 point, 1 spot)
    float4 colorIntensity; // rgb = colour, a = intensity (peak added brightness at the centre)
    float4 spotDirCone;    // xy = aim direction (2D, normalized), z = cosInner, w = cosOuter
};

struct Light2DPush
{
    DevicePtr<FrameConstantsData> frameConstants;
    DevicePtr<GpuLight2D> lights;
    uint   count;          // number of GpuLight2D records
    uint   occluderSlot;   // bindless slot of the R8 occluder mask (0xFFFFFFFF = shadows off)
    float2 viewportSize;
    float4 ambient;        // rgb = ambient floor, a = per-channel clamp ceiling
    float4 shadowParams;   // x = enabled, y = strength, z = step count, w = world-space bias
};

[[vk::push_constant]] Light2DPush pc;

struct VSOutput
{
    float4 position : SV_Position;
};

[shader("vertex")]
VSOutput vertexMain(uint vertexId : SV_VertexID)
{
    // Fullscreen quad in clip space (two triangles).
    const float2 corners[6] = {
        float2(0, 0), float2(1, 0), float2(1, 1),
        float2(0, 0), float2(1, 1), float2(0, 1),
    };
    const float2 c = corners[vertexId % 6u];
    VSOutput o;
    o.position = float4(c * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

// Fraction [0..1] of this light that reaches `world`, 1 = unshadowed. Marches the pixel->light line
// through the occluder mask in screen-UV space (ortho: the world line maps to a straight UV line).
float ShadowTerm(FrameConstantsData* frame, float2 fragUV, float2 lightPos, float worldDist)
{
    if (pc.shadowParams.x < 0.5 || pc.occluderSlot == 0xFFFFFFFFu)
    {
        return 1.0;
    }
    const float4 lc = mul(frame->viewProj, float4(lightPos, 0.0, 1.0));
    const float2 lightUV = (lc.xy / lc.w) * 0.5 + 0.5;

    const int steps = max(1, int(pc.shadowParams.z));
    // Skip the pixel's own cell so a lit wall face isn't shadowed by itself (bias in world units,
    // converted to a fraction of the ray).
    const float t0 = clamp(pc.shadowParams.w / max(worldDist, 1e-3), 0.0, 0.9);

    float occ = 0.0;
    for (int s = 0; s < steps; ++s)
    {
        const float t = lerp(t0, 1.0, (float(s) + 0.5) / float(steps));
        const float2 uv = lerp(fragUV, lightUV, t);
        occ = max(occ, g_textures[pc.occluderSlot].SampleLevel(g_linearSampler, uv, 0.0).r);
    }
    return 1.0 - occ * pc.shadowParams.y;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    FrameConstantsData* frame = pc.frameConstants.Ptr();

    // Reconstruct the world-space point under this fragment (2D ortho: world.xy is depth-independent).
    const float2 fragUV = input.position.xy / max(pc.viewportSize, float2(1.0, 1.0));
    const float2 ndc = fragUV * 2.0 - 1.0;
    const float4 wp = mul(frame->invViewProj, float4(ndc, 0.0, 1.0));
    const float2 world = wp.xy / wp.w;

    float3 light = pc.ambient.rgb;

    GpuLight2D* lights = pc.lights.Ptr();
    for (uint i = 0u; i < pc.count; ++i)
    {
        const GpuLight2D L = lights[i];
        const float2 p = L.posRadiusKind.xy;
        const float radius = max(L.posRadiusKind.z, 1e-3);
        const uint kind = uint(L.posRadiusKind.w + 0.5);

        const float d = distance(world, p);
        // Smooth quadratic rolloff: 1 at the centre, 0 at the radius. Pixel-art-friendly, no hard ring.
        float atten = saturate(1.0 - d / radius);
        atten *= atten;

        if (kind == 1u) // spot: multiply by the cone falloff
        {
            const float2 toFrag = d > 1e-4 ? (world - p) / d : L.spotDirCone.xy;
            const float2 aim = normalize(L.spotDirCone.xy + float2(1e-6, 0.0));
            const float cosAng = dot(toFrag, aim);
            atten *= smoothstep(L.spotDirCone.w, L.spotDirCone.z, cosAng); // cosOuter -> cosInner
        }

        // Only pay for shadows on lights that actually reach this pixel.
        if (atten > 0.001)
        {
            atten *= ShadowTerm(frame, fragUV, p, d);
        }

        light += L.colorIntensity.rgb * L.colorIntensity.a * atten;
    }

    // Clamp so an over-bright stack of lights saturates instead of producing NaNs/inf under multiply.
    light = min(light, float3(pc.ambient.a, pc.ambient.a, pc.ambient.a));
    return float4(light, 1.0);
}
