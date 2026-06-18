// Animated spherical plasma ball shader.
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

static const float kPI = 3.14159265358979323846;

float3 Palette(float t)
{
    float3 a = float3(0.5, 0.5, 0.5);
    float3 b = float3(0.5, 0.5, 0.5);
    float3 c = float3(1.0, 1.0, 1.0);
    float3 d = float3(0.0, 0.15, 0.25);
    return a + b * cos(6.28318 * (c * t + d));
}

// ── Fragment shader ────────────────────────────────────────────────────────

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    FrameConstantsData* fc = pc.frame.Ptr();
    float elapsed = fc->elapsedTime;
    GpuMaterial* materials = (GpuMaterial*)(fc->materialBufferAddr);

    GpuMaterial mat;
    mat.baseColorFactor = float4(1.0, 1.0, 1.0, 1.0);
    mat.metallicFactor = 1.0;
    mat.roughnessFactor = 1.0;
    mat.occlusionStrength = 1.0;
    mat.alphaCutoff = 0.5;
    mat.emissiveFactor = float4(0.0, 0.0, 0.0, 0.0);
    mat.flags = 0u;
    mat.albedoSlot = kNoTexture;
    mat.normalSlot = kNoTexture;
    mat.metallicRoughnessSlot = kNoTexture;
    mat.occlusionSlot = kNoTexture;
    mat.emissiveSlot = kNoTexture;

    if (input.materialIndex != kNoTexture)
    {
        mat = materials[input.materialIndex];
    }

    float2 uv = input.uv;

    float3 baseColor = mat.baseColorFactor.rgb;
    if (mat.albedoSlot != kNoTexture)
    {
        baseColor *= g_textures[mat.albedoSlot].Sample(g_linearSampler, uv).rgb;
    }
    float3 vertexColor = input.vertexColor;
    if (dot(vertexColor, vertexColor) < 1e-8)
    {
        vertexColor = float3(1.0, 1.0, 1.0);
    }
    baseColor *= vertexColor;

    float scale = max(mat.roughnessFactor, 0.01);
    float speed = max(mat.metallicFactor, 0.0);
    float intensity = max(mat.occlusionStrength, 0.0);
    float3 tint = mat.emissiveFactor.xyz;

    float t = elapsed * speed;

    // ── 3D normal-space plasma ───────────────────────────────────────────
    float3 n = normalize(input.worldNormal);
    float3 p = n * scale;

    float plasma =
          sin(p.x * 6.0 + t)
        + sin(p.y * 7.0 - t * 0.7)
        + sin(p.z * 5.0 + t * 0.5)
        + sin(length(p.xy) * 12.0 - t * 1.3);
    plasma *= 0.25;

    // ── Spherical flow bands ─────────────────────────────────────────────
    float longitude = atan2(n.z, n.x);
    float latitude = asin(n.y);

    float flow =
        sin(longitude * 8.0 + t * 2.0)
      + sin(latitude * 12.0 - t * 1.4);
    flow *= 0.5;

    plasma += flow * 0.4;

    // ── Cosine palette ───────────────────────────────────────────────────
    float3 plasmaColor = Palette(plasma * 0.5 + 0.5);

    float3 tintFactor = any(tint > 0.001) ? tint : float3(1.0, 1.0, 1.0);
    plasmaColor *= tintFactor;
    plasmaColor *= (0.5 + intensity * 0.5);
    plasmaColor = min(plasmaColor, float3(10.0, 10.0, 10.0));

    // ── Filament structures (bright electric veins) ──────────────────────
    float filaments = pow(saturate(1.0 - abs(plasma)), 10.0);
    plasmaColor += filaments * float3(2.0, 1.5, 3.0);

    // ── View-dependent effects ───────────────────────────────────────────
    float3 V = normalize(fc->cameraWorldPos.xyz - input.worldPos);
    float NdotV = max(dot(n, V), 0.0);

    // Stronger rim fresnel (blue energy-field rim).
    float fresnel = pow(1.0 - NdotV, 5.0);
    plasmaColor += fresnel * float3(0.4, 0.8, 2.0);

    // Fake volumetric depth glow - energy appears to live inside the sphere.
    float depthGlow = pow(1.0 - abs(dot(n, V)), 2.0);
    plasmaColor *= 1.0 + depthGlow * 0.75;

    // ── Core arcs (electrode tendrils) ───────────────────────────────────
    float3 centerDir = normalize(float3(
        sin(t * 0.4),
        cos(t * 0.3),
        sin(t * 0.2)));
    float arc = dot(n, centerDir);
    arc = pow(saturate(arc), 32.0);
    plasmaColor += arc * float3(4.0, 2.0, 8.0);

    // ── Consume remaining VSOutput fields ────────────────────────────────
    // worldTangent / tangentSign / uv2 - perturb the normal so every
    // vertex->fragment interface slot is genuinely used in the output.
    float3 T = normalize(input.worldTangent);
    float3 B = cross(n, T) * input.tangentSign;
    float2 uv2Used = input.uv2;
    float3 nrmPerturbed = normalize(n + (T * sin(elapsed * 0.5) + B * cos(elapsed * 0.3)) * 0.02);
    float perturbMix = sin(uv2Used.x * 10.0 + uv2Used.y * 7.0) * 0.01;
    nrmPerturbed = normalize(nrmPerturbed + T * perturbMix);
    float volNdotV = max(dot(nrmPerturbed, V), 0.0);
    float volGlow = pow(1.0 - volNdotV, 3.0);

    float3 finalColor = plasmaColor + volGlow * float3(0.05, 0.1, 0.2);

    return float4(finalColor, mat.baseColorFactor.a);
}
