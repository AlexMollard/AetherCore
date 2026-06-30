// Full PBR forward shader for all standard geometry.
//
// Vertex data is fetched via buffer device address (BDA) stored in
// DrawInstanceData.vertexBufferAddr.  No vertex input bindings.
//
// Push constants (24 bytes, matches aether::DrawPushConstants):
//   offset  0 : uint64_t  frameAddr         BDA -> FrameConstantsData
//   offset  8 : uint64_t  instanceDataAddr  BDA -> DrawInstanceData[]
//   offset 16 : uint64_t  skinPaletteAddr   BDA -> float4x4[] skin matrices

// ── Bindless resources (descriptor set 0) ────────────────────────────────
[[vk::binding(0, 0)]] Texture2D    g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

#include "include/FrameConstants.slangh"
#include "include/RenderContracts.slangh"
#include "include/GpuMaterial.slangh"
#include "include/DirectionalShadowCSM.slangh"
#include "include/LocalShadow.slangh"
#include "include/PBRUtilities.slangh"
#include "include/MeshVertex.slangh"
#include "include/DefaultVertex.slangh"

[[vk::push_constant]] DrawPushConstants pc;

// ── Fragment shader ───────────────────────────────────────────────────────

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    FrameConstantsData* fc        = pc.frame.Ptr();
    GpuMaterial*        materials = (GpuMaterial*)(fc->materialBufferAddr);

    GpuMaterial mat;
    mat.baseColorFactor = float4(1.0, 1.0, 1.0, 1.0);
    mat.metallicFactor = 0.0;
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

    const bool hasMaterial = input.materialIndex != kNoTexture;
    if (hasMaterial)
    {
        mat = materials[input.materialIndex];
    }

    float2 uv = input.uv;

    // ── Base colour ───────────────────────────────────────────────────────
    float4 baseColor = mat.baseColorFactor;
    if (mat.albedoSlot != kNoTexture)
    {
        baseColor *= g_textures[mat.albedoSlot].Sample(g_linearSampler, uv);
    }
    // Apply optional vertex color tint, but ignore clearly invalid all-zero streams.
    float3 vertexColor = hasMaterial ? float3(1.0, 1.0, 1.0) : input.vertexColor;
    if (any(vertexColor < 0.0) || any(vertexColor > 1.0) || dot(vertexColor, vertexColor) < 0.0025)
    {
        vertexColor = float3(1.0, 1.0, 1.0);
    }
    baseColor.rgb *= vertexColor;

    // Alpha test.
    if ((mat.flags & kFlagAlphaMask) != 0u && baseColor.a < mat.alphaCutoff)
    {
        discard;
    }

    // ── TBN - world-space tangent frame ──────────────────────────────────
    float3 Nraw = input.worldNormal;
    const float nRawLen2 = dot(Nraw, Nraw);
    float3 N;
    if (nRawLen2 > 1e-12)
    {
        N = normalize(Nraw);
    }
    else
    {
        // Fallback: screen-space dFdxy face normal.
        float3 faceN = cross(ddx(input.worldPos), ddy(input.worldPos));
        const float faceLen2 = dot(faceN, faceN);
        N = (faceLen2 > 1e-12) ? normalize(faceN) : float3(0.0, 1.0, 0.0);
    }

    float3 Traw = input.worldTangent;
    const float tRawLen2 = dot(Traw, Traw);
    float3 T = (tRawLen2 > 1e-12) ? normalize(Traw) : float3(1.0, 0.0, 0.0);
    T = T - N * dot(T, N);                         // Gram-Schmidt re-orthogonalise
    const float tLen2 = dot(T, T);
    if (tLen2 > 1e-12)
    {
        T = normalize(T);
    }
    else
    {
        float3 fallbackT = cross(float3(0.0, 1.0, 0.0), N);
        if (dot(fallbackT, fallbackT) < 1e-12)
        {
            fallbackT = cross(float3(1.0, 0.0, 0.0), N);
        }
        T = normalize(fallbackT);
    }
    float3 B = cross(N, T) * input.tangentSign;
    // Rows of TBN = T, B, N  ->  mul(tangentVec, TBN) transforms to world space.
    float3x3 TBN = float3x3(T, B, N);

    // Preserve the geometric normal so local light response can be softened
    // against tangent-space normal-map directionality.
    float3 Ngeom = N;

    if (mat.normalSlot != kNoTexture)
    {
        float3 tn = g_textures[mat.normalSlot].Sample(g_linearSampler, uv).xyz;
        tn = tn * 2.0 - 1.0;
        N = normalize(mul(tn, TBN));
    }

    // ── Metallic / Roughness ──────────────────────────────────────────────
    float metallic  = mat.metallicFactor;
    float roughness = mat.roughnessFactor;
    if (mat.metallicRoughnessSlot != kNoTexture)
    {
        // glTF ORM: R=occlusion(if packed), G=roughness, B=metallic
        float3 orm = g_textures[mat.metallicRoughnessSlot].Sample(g_linearSampler, uv).rgb;
        roughness *= orm.g;
        metallic  *= orm.b;
    }
    roughness = clamp(roughness, 0.04, 1.0);
    metallic  = saturate(metallic);

    // ── Occlusion ─────────────────────────────────────────────────────────
    float ao = 1.0;
    if (mat.occlusionSlot != kNoTexture)
    {
        float aoSample = g_textures[mat.occlusionSlot].Sample(g_linearSampler, uv).r;
        ao = lerp(1.0, aoSample, mat.occlusionStrength);
    }
    // Sample GTAO from the resource table.
    ResourceEntry aoEntry = LoadResourceEntry(fc->resourceTableAddr, kFrameResourceId_Gtao);
    if (aoEntry.type == kResourceTypeBindlessTexture && aoEntry.address != 0xFFFFFFFFu && aoEntry.width > 0u && aoEntry.height > 0u)
    {
        uint aoSlot = uint(aoEntry.address);
        float2 aoUv = input.svPosition.xy / (float2(aoEntry.width, aoEntry.height) * 2.0f);
        float gtao = g_textures[aoSlot].SampleLevel(g_linearSampler, saturate(aoUv), 0.0f).r;
        ao *= lerp(1.0f, saturate(gtao), 0.7f);
    }

    // ── Emissive ──────────────────────────────────────────────────────────
    float3 emissive = mat.emissiveFactor.xyz;
    if (mat.emissiveSlot != kNoTexture)
    {
        emissive *= g_textures[mat.emissiveSlot].Sample(g_linearSampler, uv).rgb;
    }

    // ── PBR lighting (directional + tiled local lights) ───────────────────
    float3 sunDir       = normalize(fc->sunDirectionIntensity.xyz);
    float  sunIntensity = fc->sunDirectionIntensity.w;
    float3 sunColor     = fc->sunColor.rgb;
    float3 ambientColor = fc->ambientColor.rgb;

    float3 camWorldPos = fc->cameraWorldPos.xyz;

    float3 Vraw = camWorldPos - input.worldPos;
    const float vLen2 = dot(Vraw, Vraw);
    float3 V = (vLen2 > 1e-12) ? normalize(Vraw) : N;

    // Fresnel base reflectance: dielectrics use 0.04, metals use albedo.
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), baseColor.rgb, metallic);

    // Directional sun contribution.
    float3 L = sunDir;
    float3 Hraw = V + L;
    const float hLen2 = dot(Hraw, Hraw);
    float3 H = (hLen2 > 1e-12) ? normalize(Hraw) : N;

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    float D = D_GGX(NdotH, roughness);
    float G = G_SmithSchlick(NdotV, NdotL, roughness);
    float3 F = F_Schlick(VdotH, F0);
    float3 specular = (D * G * F) / max(4.0 * NdotL * NdotV, 1e-4);

    float3 kD = (float3(1.0, 1.0, 1.0) - F) * (1.0 - metallic);
    float3 diffuse = kD * baseColor.rgb / kPI;

    const float shadow = ComputeDirectionalShadow(fc, input.worldPos, N, L);
    float3 Lo = (diffuse + specular) * (sunColor * (sunIntensity * NdotL * lerp(1.0, shadow, fc->shadowParams.z)));

    // Tiled local lights (point + spot).
    // Blend toward geometric normal so local lights remain visibly radial on
    // the ground while still retaining some normal-map detail.
    const float localNormalMapInfluence = 0.35;
    const float3 Nlocal = normalize(lerp(Ngeom, N, localNormalMapInfluence));
    uint tilePx = fc->tiledLightGridInfo.x;
    uint tilesX = fc->tiledLightGridInfo.y;
    uint tilesY = fc->tiledLightGridInfo.z;
    if (tilePx > 0u && tilesX > 0u && tilesY > 0u)
    {
        uint2 tileCoord = uint2(
            min((uint)input.svPosition.x / tilePx, tilesX - 1u),
            min((uint)input.svPosition.y / tilePx, tilesY - 1u));
        uint tile = tileCoord.y * tilesX + tileCoord.x;

        uint2 header = pc.tileHeaders.Load(tile);
        uint start = header.x;
        uint count = header.y;
        uint maxPerTile = fc->tiledLightBufferOffsets.w;
        if (maxPerTile > 0u)
        {
            count = min(count, maxPerTile);
        }
        count = min(count, 512u); // hard cap to keep worst-case bounded

        for (uint i = 0u; i < count; ++i)
        {
            uint lightIndex = pc.tileLightIndices.Load(start + i);
            LocalLight light = LoadLocalLight(pc.lightData, lightIndex);
            AccumulateLocalLight(
                light,
                input.worldPos,
                Nlocal,
                V,
                F0,
                baseColor.rgb,
                metallic,
                roughness,
                fc,
                Lo);
        }
    }

    // Image-based ambient (constant approximation - replace with IBL later).
    float3 ambient = ambientColor * baseColor.rgb * ao;

    float3 finalColor = max(Lo + ambient + emissive, float3(0.0, 0.0, 0.0));

    return float4(finalColor, baseColor.a);
}
