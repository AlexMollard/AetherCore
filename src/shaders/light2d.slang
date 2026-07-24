// 2D screen-space light-map. Runs once, fullscreen, AFTER the sprite/tile layer has been drawn into
// the scene HDR colour and BEFORE tonemap, with MULTIPLY blend (DstColor, Zero) so the hardware does
//   hdrColor.rgb *= light.rgb
// The light map is: ambient (a dark floor) + the radial contribution of every point light and the
// cone contribution of every spot light in the scene. Lit spots brighten/tint the pixel art; the rest
// sinks toward ambient. This consumes the SAME lights the 3D renderer uses (engine PointLight/SpotLight
// components) - one light concept, lighting both 2D sprites and 3D meshes.
//
// World reconstruction is exact for a 2D ortho camera: world.xy depends only on screen ndc, not depth.
#include "include/DevicePointer.slangh"
#include "include/FrameConstants.slangh"

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
    uint   pad;
    float2 viewportSize;
    float4 ambient;        // rgb = ambient floor, a = per-channel clamp ceiling
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

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    FrameConstantsData* frame = pc.frameConstants.Ptr();

    // Reconstruct the world-space point under this fragment (2D ortho: world.xy is depth-independent).
    const float2 ndc = input.position.xy / max(pc.viewportSize, float2(1.0, 1.0)) * 2.0 - 1.0;
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

        light += L.colorIntensity.rgb * L.colorIntensity.a * atten;
    }

    // Clamp so an over-bright stack of lights saturates instead of producing NaNs/inf under multiply.
    light = min(light, float3(pc.ambient.a, pc.ambient.a, pc.ambient.a));
    return float4(light, 1.0);
}
