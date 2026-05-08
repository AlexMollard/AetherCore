// skybox.slang - Procedural gradient sky, rendered as a full-screen pass
// that runs before the forward pass so all opaque geometry draws on top.
//
// Designed to grow: future passes can layer volumetric clouds, atmospheric
// scattering, stars, and a moon by extending EvalSky() or adding passes.
//
// Push constant  (8 bytes):
//   offset 0 : uint64_t  frameAddr   BDA -> FrameConstantsData
//
// No descriptor sets - all data comes from the push-constant BDA pointer.

#include "include/FrameConstants.slangh"

struct SkyboxPush
{
    uint64_t frameAddr;
};
[[vk::push_constant]] SkyboxPush pc;

// ── Vertex I/O ────────────────────────────────────────────────────────────

struct VSOut
{
    float4 svPos : SV_Position;
    float2 ndc   : TEXCOORD0;   // clip-space XY (== NDC XY since w = 1)
};

// ── Vertex shader ──────────────────────────────────────────────────────────
// Produces a full-screen CCW triangle from SV_VertexID - no vertex buffer.
//   ID 0 -> NDC (-1, -1)   top-left  (Vulkan: top-left since Y is down)
//   ID 1 -> NDC ( 3, -1)   right overshoot
//   ID 2 -> NDC (-1,  3)   bottom overshoot
// Rasteriser clips the overshoots to the viewport.

[shader("vertex")]
VSOut vertexMain(uint vertexID : SV_VertexID)
{
    const float2 uv    = float2((vertexID << 1u) & 2u, vertexID & 2u);
    const float2 ndcXY = uv * 2.0f - 1.0f;
    VSOut o;
    o.svPos = float4(ndcXY, 0.0f, 1.0f);
    o.ndc   = ndcXY;
    return o;
}

// ── Sky evaluation ────────────────────────────────────────────────────────
// Returns a linear-HDR sky colour for a given world-space view direction.
//
// horizonColor  - base colour near the horizon (typically a brightened
//                 sky-horizon tint supplied by the app.
// sunDir        - normalised world-space direction TOWARDS the sun.
// sunIntensity  - directional light intensity scalar.

float Hash31(float3 p)
{
    return frac(sin(dot(p, float3(127.1f, 311.7f, 74.3f))) * 43758.5f);
}

float Hash12(float2 p)
{
    return frac(sin(dot(p, float2(127.1f, 311.7f))) * 43758.5453f);
}

float Smooth5(float x)
{
    x = saturate(x);
    return x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f);
}

float StarField(float3 dir)
{
    const float3 cell = floor(dir * 180.0f);
    float star = smoothstep(0.997f, 1.0f, Hash31(cell));
    star *= saturate(dir.y + 0.1f); // hide below the horizon
    return star;
}

float3 EvalSky(
    float3 worldDir,
    float3 sunDir,
    float3 sunColor,
    float  sunIntensity,
    float3 horizonColor,
    float3 zenithColor,
    float3 voidColor)
{
    const float sunAltitude = saturate(sunDir.y);
    const float nightAmount = 1.0f - smoothstep(-0.10f, 0.25f, sunDir.y);
    const float dayAmount   = 1.0f - nightAmount;
    const float lowSunAmount = 1.0f - smoothstep(0.08f, 0.55f, sunAltitude);

    // Smooth horizon -> zenith gradient to avoid visible daytime band seams.
    const float t       = saturate(worldDir.y * 0.5f + 0.5f);
    const float3 warmHorizon = horizonColor + float3(0.20f, 0.08f, 0.02f) * lowSunAmount;
    const float3 zenith = zenithColor * lerp(1.0f, 0.45f, nightAmount);

    // Broader transition so the horizon rolls into the sky more gradually.
    const float gradientBlend = smoothstep(-0.10f, 0.95f, t);
    float3 sky = lerp(warmHorizon, zenith, Smooth5(gradientBlend));

    // Gentle atmospheric haze hugging the horizon to further soften the junction.
    const float horizonHaze = exp2(-18.0f * abs(worldDir.y));
    sky = lerp(sky, warmHorizon, horizonHaze * (0.16f + 0.10f * lowSunAmount));

    // Daytime cyan lift around mid-elevations to keep noon sky lively.
    const float midBand = saturate(1.0f - abs(t * 2.0f - 1.0f) * 1.35f);
    sky += float3(0.0f, 0.018f, 0.055f) * midBand * dayAmount;

    // Below the horizon: darken gently to avoid a sharp cutoff.
    const float belowHorizon = saturate(-worldDir.y * 2.0f);
    sky = lerp(sky, sky * 0.15f, belowHorizon);

    // Smooth solar radiance profile (core + corona) to avoid hard concentric bands.
    const float cosAngle = saturate(dot(worldDir, sunDir));
    const float d = 1.0f - cosAngle; // angular distance proxy near the sun

    // Multi-lobe falloff gives a natural-looking bright core with soft bloom.
    const float sunCore   = exp2(-26000.0f * d);
    const float sunInner  = exp2(-2600.0f  * d);
    const float sunOuter  = exp2(-260.0f   * d);
    const float sunCorona = exp2(-70.0f    * d);

    // Lower sun altitude shifts the disk and halo warmer.
    const float3 diskColor  = lerp(
        float3(1.6f, 0.6f, 0.2f),
        float3(1.5f, 1.35f, 1.1f),
        sunAltitude);
    const float3 haloColor  = lerp(
        float3(1.0f, 0.3f, 0.05f),
        float3(1.0f, 0.65f, 0.25f),
        sunAltitude);

    // Keep the core bright and warm, while the wider corona stays softer.
    const float3 sunCoreColor = diskColor * sunColor;
    const float3 sunHaloColor = haloColor * sunColor;
    sky += sunCoreColor * sunIntensity * ((2.8f * sunCore) + (0.90f * sunInner));
    sky += sunHaloColor * sunIntensity * ((0.28f * sunOuter) + (0.10f * sunCorona));

    // The sky brightens along the entire horizon in the sun's azimuth,
    // not just at the solar disk itself.
    const float2 worldDirXZ = worldDir.xz / max(length(worldDir.xz), 1e-4f);
    const float2 sunDirXZ   = sunDir.xz   / max(length(sunDir.xz),   1e-4f);
    const float  azimuthDot = dot(worldDirXZ, sunDirXZ);
    const float  horizonMask = saturate(1.0f - abs(worldDir.y) * 6.0f);
    const float  horizonGlow = smoothstep(0.55f, 1.0f, azimuthDot) * horizonMask * (0.35f * lowSunAmount);
    sky += horizonGlow * lerp(
        float3(1.0f, 0.4f, 0.1f),
        float3(1.0f, 0.8f, 0.5f),
        sunAltitude) * sunColor * sunIntensity;

    // Subtle forward-scatter streak near sunrise/sunset (not a lens flare).
    const float streakHorizonMask = smoothstep(-0.03f, 0.20f, worldDir.y) *
                                    (1.0f - smoothstep(0.20f, 0.55f, worldDir.y));
    const float streakAzimuth = smoothstep(0.70f, 1.0f, azimuthDot);
    const float streakForward = smoothstep(0.86f, 1.0f, cosAngle);
    const float sunStreak = 0.08f * lowSunAmount * streakHorizonMask * streakAzimuth * streakForward;
    sky += sunHaloColor * sunIntensity * sunStreak;

    // At night, darken the sky aggressively to make the cycle more obvious.
    sky *= lerp(1.0f, 0.18f, nightAmount);

    // Cheap night stars.
    sky += StarField(worldDir) * nightAmount * float3(0.9f, 0.95f, 1.0f);

    // Force a near-black void below the horizon.
    // Keep a small transition band to avoid hard aliasing at exactly y=0.
    const float voidBlend = 1.0f - smoothstep(-0.22f, -0.015f, worldDir.y);
    sky = lerp(sky, voidColor, voidBlend);

    return sky;
}

// ── Fragment shader ────────────────────────────────────────────────────────

[shader("fragment")]
float4 fragmentMain(VSOut input) : SV_Target0
{
    FrameConstantsData* fc = (FrameConstantsData*)pc.frameAddr;

    // ── Step 1: NDC -> view-space direction ──────────────────────────────
    // GLM perspective (Vulkan variant) column-major layout:
    //   proj[0][0]  = 1 / (aspect * tan(fovY/2))   - col 0, row 0
    //   proj[1][1]  = -1 / tan(fovY/2)              - col 1, row 1  (Y-flip)
    //   proj[2][3]  = -1                             - col 2, row 3  (w = -viewZ)
    //
    // Inverting the perspective divide:
    //   ndc.x = proj[0][0] * viewX  ->  viewX = ndc.x / proj[0][0]
    //   ndc.y = proj[1][1] * viewY  ->  viewY = ndc.y / proj[1][1]
    //   viewZ = -1  (ray shoots into the scene)
    const float viewX = input.ndc.x / fc->proj[0][0];
    const float viewY = input.ndc.y / fc->proj[1][1];
    const float3 viewDir = normalize(float3(viewX, viewY, -1.0f));

    // ── Step 2: view-space -> world-space ────────────────────────────────
    // fc->view is the world->view rotation R (column-major, same convention
    // as model matrix). The inverse of an orthogonal rotation is its
    // transpose, which is equivalent to multiplying the view-space row
    // vector by R directly.
    const float3x3 viewRot   = float3x3(fc->view[0].xyz, fc->view[1].xyz, fc->view[2].xyz);
    const float3   worldDir  = normalize(mul(viewDir, viewRot));

    // ── Step 3: evaluate sky ─────────────────────────────────────────────
    const float3 sunDir       = normalize(fc->sunDirectionIntensity.xyz);
    const float  sunIntensity = fc->sunDirectionIntensity.w;
    const float3 sunColor     = fc->sunColor.rgb;
    const float3 horizonColor = fc->skyHorizonColor.rgb;
    const float3 zenithColor  = fc->skyZenithColor.rgb;
    const float3 voidColor    = fc->skyVoidColor.rgb;

    float3 sky = EvalSky(
        worldDir,
        sunDir,
        sunColor,
        sunIntensity,
        horizonColor,
        zenithColor,
        voidColor);

    // Tiny dither helps hide residual daytime gradient quantization.
    const float dayAmount = saturate(sunDir.y * 0.5f + 0.5f);
    const float dither = Hash12(floor(input.svPos.xy)) - 0.5f;
    sky += dither * (1.0f / 1024.0f) * dayAmount;

    return float4(max(sky, float3(0.0f, 0.0f, 0.0f)), 1.0f);
}
