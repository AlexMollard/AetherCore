// plasma.slang - Animated time-based plasma shader.
//
// Reinterprets GpuMaterial fields for effect control:
//   emissiveFactor.xyz = tint (color modulation)
//   roughnessFactor     = pattern scale
//   metallicFactor      = animation speed
//   occlusionStrength   = intensity multiplier
//
// Uses FrameConstantsData.elapsedTime for time-driven animation.

[[vk::binding(0, 0)]] Texture2D    g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

#include "include/FrameConstants.slangh"
#include "include/RenderContracts.slangh"
#include "include/GpuMaterial.slangh"
#include "include/MeshVertex.slangh"
#include "include/DefaultVertex.slangh"

[[vk::push_constant]] DrawPushConstants pc;

// ── Fragment shader (plasma effect) ────────────────────────────────────────

static const float kPI = 3.14159265358979323846;

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    FrameConstantsData* fc = (FrameConstantsData*)(pc.frameAddr);
    GpuMaterial* materials = (GpuMaterial*)(fc->materialBufferAddr);

    GpuMaterial mat;
    mat.baseColorFactor = float4(1.0, 1.0, 1.0, 1.0);
    mat.metallicFactor = 1.0;
    mat.roughnessFactor = 1.0;
    mat.occlusionStrength = 1.0;
    mat.alphaCutoff = 0.5;
    mat.emissiveFactor = float4(0.0, 0.0, 0.0, 0.0);
    mat.flags = 0u;
    mat.albedoSlot = 0xFFFFFFFFu;
    mat.normalSlot = 0xFFFFFFFFu;
    mat.metallicRoughnessSlot = 0xFFFFFFFFu;
    mat.occlusionSlot = 0xFFFFFFFFu;
    mat.emissiveSlot = 0xFFFFFFFFu;

    if (input.materialIndex != 0xFFFFFFFFu)
    {
        mat = materials[input.materialIndex];
    }

    float3 tint     = mat.emissiveFactor.xyz;
    float  scale    = max(mat.roughnessFactor, 0.01);
    float  speed    = max(mat.metallicFactor, 0.0);
    float  intensity = max(mat.occlusionStrength, 0.0);

    float elapsed = fc->elapsedTime;

    float2 uv = input.uv;

    float3 baseColor = mat.baseColorFactor.rgb;
    if (mat.albedoSlot != 0xFFFFFFFFu)
    {
        baseColor *= g_textures[mat.albedoSlot].Sample(g_linearSampler, uv).rgb;
    }
    float3 vertexColor = input.vertexColor;
    if (dot(vertexColor, vertexColor) < 1e-8)
    {
        vertexColor = float3(1.0, 1.0, 1.0);
    }
    baseColor *= vertexColor;

    float2 p = uv * scale;
    float t = elapsed * speed;

    float v1 = sin(p.x * 3.0 + t);
    float v2 = sin(p.y * 2.5 + t * 0.8);
    float v3 = sin((p.x + p.y) * 2.0 + t * 0.6);
    float v4 = sin(sqrt(p.x * p.x + p.y * p.y) * 4.0 - t * 0.5);

    float plasma = (v1 + v2 + v3 + v4) * 0.25;

    float3 colorA = float3(0.2, 0.0, 0.6);
    float3 colorB = float3(1.0, 0.2, 0.8);
    float3 colorC = float3(0.0, 0.8, 1.0);
    float3 colorD = float3(1.0, 0.8, 0.2);

    float tA = saturate(plasma * 0.5 + 0.5);
    float tB = saturate((plasma + 0.3) * 0.5 + 0.5);
    float tC = saturate((plasma * 0.8 + 0.2) * 0.5 + 0.5);

    float3 plasmaColor = lerp(colorA, colorB, tA);
    plasmaColor = lerp(plasmaColor, colorC, tB);
    plasmaColor = lerp(plasmaColor, colorD, tC);

    float3 tintFactor = any(tint > 0.001) ? tint : float3(1.0, 1.0, 1.0);
    plasmaColor *= tintFactor;
    plasmaColor *= (0.5 + intensity * 0.5);
    plasmaColor = min(plasmaColor, float3(10.0, 10.0, 10.0));

    float3 finalColor = lerp(plasmaColor, baseColor * plasmaColor, 0.15);

    return float4(finalColor, mat.baseColorFactor.a);
}
