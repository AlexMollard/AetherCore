// Molten core effect shader.
//
// A cracked obsidian crust broken by flowing, blindingly hot veins that drift
// and breathe. Reads per-entity EffectParams from the EffectParamBuffer, indexed
// by the per-entity effectParamIndex varying:
//   tint      = molten glow colour (ember hue)
//   speed     = convective flow rate of the veins
//   scale     = crack frequency (bigger = finer cells)
//   intensity = heat / emissive strength (drives HDR bloom)
//
// A deliberate visual counterpoint to plasma.slang: where plasma is a bright,
// full-surface electric field, molten is a DARK surface pierced by narrow
// incandescent cracks - so the two effects read as clearly different systems
// while exercising the same per-entity EffectParams path.

[[vk::binding(0, 0)]] Texture2D    g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

#include "include/FrameConstants.slangh"
#include "include/RenderContracts.slangh"
#include "include/EffectParams.slangh"
#include "include/MeshVertex.slangh"
#include "include/DefaultVertex.slangh"

[[vk::push_constant]] DrawPushConstants pc;

// ── Cheap hash-based value noise (no texture reads, matching plasma's self-
//    contained style). Good enough for organic lava cells at sphere scale. ──
float Hash31(float3 p)
{
    p = frac(p * 0.3183099 + float3(0.1, 0.2, 0.3));
    p *= 17.0;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float ValueNoise(float3 x)
{
    float3 i = floor(x);
    float3 f = frac(x);
    f = f * f * (3.0 - 2.0 * f); // smoothstep fade

    float n000 = Hash31(i + float3(0.0, 0.0, 0.0));
    float n100 = Hash31(i + float3(1.0, 0.0, 0.0));
    float n010 = Hash31(i + float3(0.0, 1.0, 0.0));
    float n110 = Hash31(i + float3(1.0, 1.0, 0.0));
    float n001 = Hash31(i + float3(0.0, 0.0, 1.0));
    float n101 = Hash31(i + float3(1.0, 0.0, 1.0));
    float n011 = Hash31(i + float3(0.0, 1.0, 1.0));
    float n111 = Hash31(i + float3(1.0, 1.0, 1.0));

    float nx00 = lerp(n000, n100, f.x);
    float nx10 = lerp(n010, n110, f.x);
    float nx01 = lerp(n001, n101, f.x);
    float nx11 = lerp(n011, n111, f.x);
    float nxy0 = lerp(nx00, nx10, f.y);
    float nxy1 = lerp(nx01, nx11, f.y);
    return lerp(nxy0, nxy1, f.z);
}

float Fbm(float3 p)
{
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; ++i)
    {
        sum += amp * ValueNoise(p);
        p *= 2.02;
        amp *= 0.5;
    }
    return sum;
}

// ── Fragment shader ────────────────────────────────────────────────────────

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    FrameConstantsData* fc = pc.frame.Ptr();
    float elapsed = fc->elapsedTime;

    EffectParams fx;
    fx.tint = float4(1.0, 0.35, 0.05, 1.0);
    fx.speed = 1.0;
    fx.scale = 1.0;
    fx.intensity = 1.0;
    // Guard on the BDA too: passes that do not wire the effect buffer (e.g.
    // render-to-texture camera views) ship effectParamBufferAddr == 0, and
    // dereferencing it would be a null read. Fall back to default params there.
    if (input.effectParamIndex != 0xFFFFFFFFu && fc->effectParamBufferAddr != 0)
    {
        EffectParams* params = (EffectParams*)(fc->effectParamBufferAddr);
        fx = params[input.effectParamIndex];
    }

    float scale = max(fx.scale, 0.01);
    float speed = max(fx.speed, 0.0);
    float intensity = max(fx.intensity, 0.0);
    float3 glow = fx.tint.rgb;

    float t = elapsed * speed;
    float3 n = normalize(input.worldNormal);

    // ── Domain-warped fbm over the surface normal: organic, slowly churning
    //    lava cells. The warp makes the veins wander instead of reading as a
    //    static noise field; the drift term convects them "upward". ──────────
    float3 q = n * (scale * 2.5);
    float3 drift = float3(0.0, t * 0.15, t * 0.05);
    float warp = Fbm(q + drift);
    float cells = Fbm(q * 1.7 + warp * 1.2 + drift);

    // ── Crust vs. molten split. Low fbm = cool basalt crust, high = hot rock.
    //    A pulsing threshold makes the whole surface breathe. ────────────────
    float pulse = 0.5 + 0.5 * sin(t * 1.3);
    float heat = smoothstep(0.35, 0.75, cells + pulse * 0.12);

    // ── Narrow incandescent veins where fbm crosses its mid level-set. ──────
    float ridge = abs(cells - 0.5);
    float veins = pow(saturate(1.0 - ridge * 6.0), 4.0);

    // ── Colour ramp: near-black rock -> deep ember -> bright ember -> core. ─
    float3 crust     = float3(0.02, 0.015, 0.02);
    float3 emberLow  = glow * 0.35;
    float3 emberHigh = glow;
    float3 whiteHot  = lerp(glow, float3(1.6, 1.4, 1.1), 0.6);

    float3 col = crust;
    col = lerp(col, emberLow,  smoothstep(0.25, 0.55, heat));
    col = lerp(col, emberHigh, smoothstep(0.55, 0.85, heat));
    col += veins * whiteHot * (0.6 + intensity);

    // ── Emissive boost so the hot regions bloom (HDR > 1). ──────────────────
    col *= (0.4 + intensity * 1.1);
    col += emberHigh * heat * heat * intensity * 1.5;

    // ── View-dependent heat rim (bright edge where the crust thins). ────────
    float3 V = normalize(fc->cameraWorldPos.xyz - input.worldPos);
    float NdotV = max(dot(n, V), 0.0);
    float rim = pow(1.0 - NdotV, 4.0);
    col += rim * glow * (0.3 + heat * 0.8);

    // ── Consume the remaining VSOutput interpolants so every vertex->fragment
    //    slot is genuinely used (parity with plasma.slang). worldTangent /
    //    tangentSign build a bitangent that adds view-grazing shimmer; uv2
    //    drives a fine flicker along the veins. ──────────────────────────────
    float3 T = normalize(input.worldTangent);
    float3 B = cross(n, T) * input.tangentSign;
    float2 uv2Used = input.uv2;
    float flicker = 0.5 + 0.5 * sin((uv2Used.x + uv2Used.y) * 30.0 + t * 6.0);
    col += veins * flicker * glow * 0.15;
    col += max(dot(B, V), 0.0) * glow * 0.05;

    col = min(col, float3(12.0, 12.0, 12.0));
    return float4(col, fx.tint.a);
}
