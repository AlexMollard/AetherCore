// Full PBR forward shader for all standard geometry.
//
// Vertex data is fetched via buffer device address (BDA) stored in
// DrawInstanceData.vertexBufferAddr.  No vertex input bindings.
//
// Push constants (48 bytes, matches aether::DrawContracts::PushConstants):
//   offset  0 : uint64_t  frameAddr         BDA -> FrameConstantsData
//   offset  8 : uint64_t  instanceDataAddr  BDA -> DrawInstanceData[]
//   offset 16 : uint64_t  skinPaletteAddr   BDA -> float4x4[] skin matrices
//   offset 24 : uint64_t  lightDataAddr      BDA -> LocalLight[]
//   offset 32 : uint64_t  tileHeadersAddr    BDA -> uint2[]
//   offset 40 : uint64_t  tileLightIndices   BDA -> uint[]

// ── Bindless resources (descriptor set 0) ────────────────────────────────
[[vk::binding(0, 0)]] Texture2D    g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

#include "include/FrameConstants.slangh"
#include "include/RenderContracts.slangh"
#include "include/GpuMaterial.slangh"
#include "include/Sky.slangh"
#include "include/HeightFog.slangh"
#include "include/ShadowSampling.slangh"
#include "include/DirectionalShadowCSM.slangh"
#include "include/ContactShadow.slangh"
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
    // Vertex color modulates base color only when the material opts in
    // (primitive meshes with meaningful vertex colors). Painted materials
    // clear the flag; material-less draws keep the legacy fallback.
    // Clearly invalid all-zero/out-of-range streams are ignored.
    float3 vertexColor = float3(1.0, 1.0, 1.0);
    if (!hasMaterial || (mat.flags & kFlagModulateVertexColor) != 0u)
    {
        vertexColor = input.vertexColor;
    }
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
        N = normalize(lerp(Ngeom, N, 0.60f));
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

    // ── Geometric specular antialiasing ───────────────────────────────────
    // Kaplanyan et al., HPG 2016, "Filtering Distributions of Normals for
    // Shading Antialiasing".
    //
    // A pixel does not see one normal, it sees every normal inside its
    // footprint. Where the surface bends or the normal map varies faster than
    // the pixel grid - ground at a grazing angle, dense detail in the distance,
    // any curved silhouette - a near-smooth material lands its specular lobe on
    // wildly different answers in neighbouring pixels. That is the sparkle that
    // crawls whenever anything moves, and no post-process antialiasing can
    // touch it: FXAA reads the shimmer as real detail, and it is different
    // every frame anyway.
    //
    // The fix is to widen the lobe rather than shrink the pixel. Measure how
    // much the normal varies across this pixel from its screen-space
    // derivatives, and fold that variance into the roughness, so the material
    // is as rough as its footprint actually is and the highlight resolves
    // instead of aliasing. Distant geometry converges to its average response
    // rather than picking one sample of it.
    {
        const float3 dNdx = ddx(N);
        const float3 dNdy = ddy(N);
        // Half-pixel Gaussian filter width; 1/(2*pi) is the paper's fit.
        const float kSigma2 = 0.15915494;
        // Ceiling on the roughness that may be added, so a silhouette or a
        // crease cannot turn the whole material matte.
        const float kKappa = 0.18;
        const float variance = kSigma2 * (dot(dNdx, dNdx) + dot(dNdy, dNdy));
        const float kernelRoughness = min(2.0 * variance, kKappa);
        roughness = sqrt(saturate(roughness * roughness + kernelRoughness));
    }

    // ── Occlusion ─────────────────────────────────────────────────────────
    float materialAo = 1.0;
    if (mat.occlusionSlot != kNoTexture)
    {
        float aoSample = g_textures[mat.occlusionSlot].Sample(g_linearSampler, uv).r;
        materialAo = lerp(1.0, aoSample, mat.occlusionStrength);
    }
    float gtaoVisibility = 1.0;

    // Sample GTAO from the resource table.
    ResourceEntry aoEntry = LoadResourceEntry(fc->resourceTableAddr, kFrameResourceId_Gtao);
    if (aoEntry.type == kResourceTypeBindlessTexture && aoEntry.address != 0xFFFFFFFFu && aoEntry.width > 0u && aoEntry.height > 0u)
    {
        uint aoSlot = uint(aoEntry.address);
        // Map full-res pixel coords to [0,1] using the actual scene extent.
        // The AO image is half-res with rounded-up dimensions, so aoEntry*2 can
        // overshoot the true extent by a pixel when the scene extent is odd.
        ResourceEntry sceneDepthEntry = LoadResourceEntry(fc->resourceTableAddr, kFrameResourceId_SceneDepth);
        float2 fullExtent = (sceneDepthEntry.type == kResourceTypeBindlessTexture && sceneDepthEntry.width > 0u && sceneDepthEntry.height > 0u)
            ? float2(sceneDepthEntry.width, sceneDepthEntry.height)
            : float2(aoEntry.width, aoEntry.height) * 2.0f;
        float2 aoUv = saturate(input.svPosition.xy / fullExtent);
        gtaoVisibility = saturate(g_textures[aoSlot].Sample(g_linearSampler, aoUv).r);
    }
    float ao = saturate(materialAo * gtaoVisibility);
    ao = saturate(ao);

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

    // Occlusion - screen-space and the material's baked map alike - is an estimate
    // of INDIRECT visibility, so it modulates ambient only. Direct visibility is
    // what the shadow maps already answer, exactly and per light. Attenuating the
    // sun and the local lights by it as well darkened lit surfaces near corners
    // until they met the shadowed ones in the middle, which read as the AO
    // cancelling the shadows out. The clamps made it worse in both directions: a
    // shadow bottoms out at 0.08 while AO could take a *lit* pixel to 0.05, so in
    // a corner the contrast could invert outright. This also matches the glTF
    // spec, where the occlusion texture applies to indirect light.
    const float ambientAo = ao;
    float3 diffuse = kD * baseColor.rgb / kPI;

    const float shadow = ComputeDirectionalShadow(fc, input.worldPos, Ngeom, L, input.svPosition.xy);
    // Materials can opt out of shadow receiving (flag set == opt out); such
    // surfaces stay fully lit by the sun regardless of the shadow map.
    const bool receivesShadows = (mat.flags & kFlagNoReceiveShadows) == 0u;
    // Contact shadows sharpen only where the cascade cannot resolve: the last few
    // centimetres at the base of an object, which a cascade texel is simply too
    // coarse to contain. Combined by min, because either occluder blocking the sun
    // is enough - a surface is not lit twice over.
    const float contact = (fc->shadowParams.w > 0.0f && receivesShadows)
        ? ContactShadow(input.worldPos, Ngeom, L, fc, input.svPosition.xy)
        : 1.0f;
    const float shadowVisibility = receivesShadows ? max(min(shadow, contact), 0.08f) : 1.0f;
    float3 Lo = (diffuse + specular) * (sunColor * (sunIntensity * NdotL * lerp(1.0, shadowVisibility, fc->shadowParams.z)));

    // Local lights accumulate separately from the sun so each keeps its own
    // shadow term; neither carries an occlusion factor.
    float3 LoLocal = float3(0.0, 0.0, 0.0);

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
                input.svPosition.xy,
                LoLocal);
        }
    }
    Lo += LoLocal;

    // Image-based ambient from the same procedural sky the skybox draws, rather than
    // one flat colour in every direction. A constant ambient gives an object no shape
    // at all where the sun does not reach: the underside of an arm reads exactly as
    // bright as the top of a shoulder. Sampling the sky along the normal restores that,
    // and costs two evaluations of a closed-form gradient - no probes, no prefiltering,
    // no cubemap.
    //
    // SkyGradient, never EvalSky: the latter carries the solar disk, and lighting a
    // surface with it would add the sun a second time on top of the direct term and
    // smear a duplicate sun across every glossy surface.
    const float3 skyDiffuse = SkyGradient(N, sunDir, sunColor, sunIntensity, fc->skyHorizonColor.rgb, fc->skyZenithColor.rgb, fc->skyVoidColor.rgb);

    // Specular probe along the reflection vector, drifting toward the diffuse sample
    // as the surface roughens - a one-sample stand-in for a prefiltered mip chain.
    const float3 R = reflect(-V, N);
    const float3 skySpecularDir = normalize(lerp(R, N, roughness * roughness));
    const float3 skySpecular = SkyGradient(skySpecularDir, sunDir, sunColor, sunIntensity, fc->skyHorizonColor.rgb, fc->skyZenithColor.rgb, fc->skyVoidColor.rgb);

    // Karis' analytic fit to the split-sum environment BRDF, so the Fresnel and
    // roughness response are right without shipping a lookup texture.
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    const float4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    const float2 envBrdf = float2(-1.04, 1.04) * a004 + r.zw;

    // ambientColor stays in as the scene's authored floor - the light that is not the
    // sky at all - so an indoor or stylised scene can still set its own base level.
    const float3 ambientDiffuse = (ambientColor + skyDiffuse) * baseColor.rgb * kD;
    const float3 ambientSpecular = skySpecular * (F0 * envBrdf.x + envBrdf.y);
    // Diffuse and specular are not occluded by the same amount. Diffuse gathers the
    // whole hemisphere, so ambient occlusion describes it directly. A specular lobe
    // gathers a cone whose width is set by roughness: the smoother the surface, the
    // narrower that cone, and the less likely the geometry that occludes the
    // hemisphere also occludes the one direction the reflection comes from. Applying
    // the diffuse term to both - which is what this did - over-darkens reflections on
    // smooth surfaces inside any crease or contact, so polished materials go dull
    // exactly where they should still catch light.
    //
    // Lagarde's approximation ("Moving Frostbite to Physically Based Rendering")
    // reconstructs the cone's occlusion from the hemisphere's. At roughness 1 it
    // returns the plain AO, because then the lobe IS the hemisphere.
    const float specularAo = saturate(pow(NdotV + ambientAo, exp2(-16.0 * roughness - 1.0)) - 1.0 + ambientAo);
    float3 ambient = ambientDiffuse * ambientAo + ambientSpecular * specularAo;

    float3 finalColor = max(Lo + ambient + emissive, float3(0.0, 0.0, 0.0));

    // Atmospheric perspective. Applied here rather than as a separate screen pass
    // because the world position and the sky terms this needs are already in hand;
    // a fullscreen pass would reconstruct both from depth to reach the same answer.
    const float fogAmount = HeightFogAmount(
        fc->cameraWorldPos.xyz,
        input.worldPos,
        fc->fogParams.x,
        fc->fogParams.y,
        fc->fogParams.w);
    if (fogAmount > 0.0f)
    {
        const float3 fogViewDir = normalize(input.worldPos - fc->cameraWorldPos.xyz);
        const float3 fogColor = HeightFogColor(
            fogViewDir,
            sunDir,
            sunColor,
            sunIntensity,
            fc->skyHorizonColor.rgb,
            fc->skyZenithColor.rgb,
            fc->skyVoidColor.rgb,
            fc->fogParams.z);
        finalColor = lerp(finalColor, fogColor, fogAmount);
    }

    // Alpha carries scene coverage for the background compositor: opaque forward
    // geometry always fully covers (the forward pass has no transparent path).
    return float4(finalColor, 1.0);
}
