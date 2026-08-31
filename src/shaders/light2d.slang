// 2D screen-space light-map. Runs once, fullscreen, AFTER the sprite/tile layer has been drawn into
// the scene HDR colour and BEFORE tonemap, with MULTIPLY blend (DstColor, Zero) so the hardware does
//   hdrColor.rgb *= light.rgb
// The light map is: ambient (a dark floor) + the radial contribution of every point light and the
// cone contribution of every spot light, DIRECTIONALLY shaded off a surface normal derived from the
// occluder mask (so geometry looks sculpted and light stops at solids) and cast-shadowed by walls.
// Consumes the SAME lights the 3D renderer uses (engine PointLight/SpotLight components).
//
// World reconstruction is exact for a 2D ortho camera: world.xy depends only on screen ndc, not depth.
#include "include/DevicePointer.slangh"
#include "include/FrameConstants.slangh"

[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

// Packed light record (64 bytes). Mirrors aether::GpuLight2D in Light2DCompositor.cpp.
struct GpuLight2D
{
    float4 posRadiusKind;  // xy = world position, z = radius (world units), w = kind (0 point, 1 spot)
    float4 colorIntensity; // rgb = colour, a = intensity (peak added brightness at the centre)
    float4 spotDirCone;    // xy = aim direction (2D, normalized), z = cosInner, w = cosOuter
    float4 flags;          // x = castsShadow (0/1)
};

struct Light2DPush
{
    DevicePtr<FrameConstantsData> frameConstants;
    DevicePtr<GpuLight2D> lights;
    uint   count;          // number of GpuLight2D records
    uint   occluderSlot;   // bindless slot of the R8 occluder mask (0xFFFFFFFF = no mask)
    float2 viewportSize;
    float4 ambient;        // rgb = ambient floor, a = per-channel clamp ceiling
    float4 shadowParams;   // x = cast-shadows enabled, y = strength, z = softness, w = world bias
    float4 shaftParams;    // x = light-shaft strength (0 = off)
};

static const int kShadowSteps = 16;

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

float SampleOccluder(float2 uv)
{
    return g_textures[pc.occluderSlot].SampleLevel(g_linearSampler, uv, 0.0).r;
}

// Cast-shadow term [0..1], 1 = unshadowed. Marches the pixel->light line through the occluder mask in
// screen-UV space (ortho: the world line maps to a straight UV line), with perpendicular taps that widen
// toward the light for a soft penumbra. Surface/interior darkening is done by the N.L term in the
// fragment; here we only occlude line-of-sight, with a modest bias so a lit face doesn't self-shadow.
// Visibility of the light AND how much of the path to it is open air.
//
// The beam you see between two occluders is light scattered toward the eye by the air along
// that line, so its brightness is the fraction of the pixel-to-light segment that is clear -
// which is exactly what this march already walks. Accumulating it here rather than in a second
// loop means the shafts cost a few instructions instead of another sixteen texture samples.
float ShadowTerm(float2 fragUV, float2 lightUV, float worldDist, out float openFraction)
{
    const float2 ray = lightUV - fragUV;
    const float2 perp = normalize(float2(-ray.y, ray.x) + float2(1e-6, 0.0));
    const float softness = max(pc.shadowParams.z, 0.0);
    const float strength = pc.shadowParams.y;
    const float t0 = clamp(0.35 / max(worldDist, 1e-3), 0.0, 0.6); // ~0.35 world-unit bias

    float occ = 0.0;
    float openSum = 0.0;
    for (int s = 0; s < kShadowSteps; ++s)
    {
        const float t = lerp(t0, 1.0, (float(s) + 0.5) / float(kShadowSteps));
        const float2 uv = lerp(fragUV, lightUV, t);
        const float spread = softness * 0.005 * t;
        const float c = SampleOccluder(uv);
        const float a = SampleOccluder(uv + perp * spread);
        const float b = SampleOccluder(uv - perp * spread);
        const float blocked = (c * 2.0 + a + b) * 0.25;
        occ = max(occ, blocked);
        // Weighted toward the light: air nearer the source is lit more brightly and so
        // scatters more, which is what gives a shaft its taper instead of a flat slab.
        openSum += (1.0 - saturate(blocked)) * t;
    }
    openFraction = openSum / (float(kShadowSteps) * 0.5);
    return 1.0 - saturate(occ) * strength;
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

    const bool haveMask = pc.occluderSlot != 0xFFFFFFFFu;
    const bool doShadows = pc.shadowParams.x > 0.5 && haveMask;

    // Derive a surface from the occluder field so lights shade DIRECTIONALLY (sculpted, not flat) and
    // stop at solids. Sample the mask at world-space offsets: uv deltas for +1 world unit in x/y (ortho
    // makes this basis uniform, but computing it per-pixel is cheap and robust to the projection).
    float2 nrm = float2(0.0, 0.0); // outward surface normal (toward air), world space
    float solidMask = 0.0;         // 1 = this pixel is inside a solid tile
    float surfaceness = 0.0;       // 1 = a well-defined surface, 0 = flat / open / symmetric
    float2 duvX = float2(0.0, 0.0); // uv delta for +1 world unit in x
    float2 duvY = float2(0.0, 0.0); // uv delta for +1 world unit in y
    if (haveMask)
    {
        const float4 cX = mul(frame->viewProj, float4(world + float2(1.0, 0.0), 0.0, 1.0));
        const float4 cY = mul(frame->viewProj, float4(world + float2(0.0, 1.0), 0.0, 1.0));
        duvX = (cX.xy / cX.w) * 0.5 + 0.5 - fragUV;
        duvY = (cY.xy / cY.w) * 0.5 + 0.5 - fragUV;

        const float self = SampleOccluder(fragUV);
        const float mR = SampleOccluder(fragUV + duvX * 0.35);
        const float mL = SampleOccluder(fragUV - duvX * 0.35);
        const float mU = SampleOccluder(fragUV + duvY * 0.35);
        const float mD = SampleOccluder(fragUV - duvY * 0.35);
        const float2 grad = float2(mR - mL, mU - mD); // points INTO solid
        const float gmag = length(grad);
        nrm = gmag > 1e-4 ? -grad / gmag : float2(0.0, 0.0);
        solidMask = smoothstep(0.3, 0.6, self);
        surfaceness = smoothstep(0.05, 0.35, gmag);
    }

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

        const float atten2 = atten; // distance/cone falloff before shadow and surface terms

        if (atten <= 0.001)
        {
            continue;
        }

        // Directional (half-Lambert N.L) shading: a solid surface takes light by how much it faces the
        // light, so geometry looks sculpted. Open air (solidMask 0) and thin/symmetric solids
        // (surfaceness 0) stay fully lit.
        if (haveMask)
        {
            const float2 Ldir = d > 1e-4 ? (p - world) / d : float2(0.0, 1.0);
            const float ndl = saturate(dot(nrm, Ldir) * 0.5 + 0.5);
            float surf = lerp(1.0, ndl, surfaceness);

            // Depth-into-solid TOWARD THIS LIGHT: if the material just in front of the pixel (on the way
            // to the light) is also solid, the pixel is behind the surface and gets no light. Sampling
            // along the light direction - rather than at a fixed radius - is what keeps light from
            // seeping under the ground WITHOUT painting a dark band parallel to the terrain.
            const float2 stepUV = (duvX * Ldir.x + duvY * Ldir.y) * 0.45; // ~0.45 world units toward the light
            const float s1 = SampleOccluder(fragUV + stepUV);
            const float s2 = SampleOccluder(fragUV + stepUV * 2.0);
            const float behind = saturate(max(s1, s2 * 0.65));
            surf *= 1.0 - behind;

            atten *= lerp(1.0, surf, solidMask);
        }

        // Cast shadows for shadow-casting lights that still reach this pixel.
        //
        // The scatter fraction starts at 1 - fully open air - because a light that does not
        // cast shadows has nothing occluding it by the author's own choice, and its beam
        // should not silently disappear. The march below replaces this with what it measures
        // for the lights that DO cast, so scattering follows the same rule the light does.
        float openFraction = 1.0;
        if (atten > 0.001 && L.flags.x > 0.5 && doShadows)
        {
            const float4 lc = mul(frame->viewProj, float4(p, 0.0, 1.0));
            const float2 lightUV = (lc.xy / lc.w) * 0.5 + 0.5;
            atten *= ShadowTerm(fragUV, lightUV, d, openFraction);
        }

        light += L.colorIntensity.rgb * L.colorIntensity.a * atten;

        // Light scattered by the air on the way here. It is added rather than multiplied
        // because it is light arriving from the medium itself, not light this surface
        // reflects - and it is suppressed inside solids, which have no air in them.
        if (pc.shaftParams.x > 0.001)
        {
            const float shaft = openFraction * pc.shaftParams.x * (1.0 - solidMask);
            light += L.colorIntensity.rgb * L.colorIntensity.a * shaft * atten2;
        }
    }

    // Clamp so an over-bright stack of lights saturates instead of producing NaNs/inf under multiply.
    light = min(light, float3(pc.ambient.a, pc.ambient.a, pc.ambient.a));
    return float4(light, 1.0);
}
