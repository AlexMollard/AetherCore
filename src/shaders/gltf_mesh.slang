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
#include "include/EnvBrdf.slangh"
#include "include/AmbientOcclusion.slangh"
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

    // Variation the normal map lost to mip filtering, recovered below as roughness.
    // Averaging conflicting normals shortens the result; the shortfall is Toksvig's measure
    // of the detail that averaging removed.
    float normalMapVariance = 0.0;
    if (mat.normalSlot != kNoTexture)
    {
        float3 tn = g_textures[mat.normalSlot].Sample(g_linearSampler, uv).xyz;
        tn = tn * 2.0 - 1.0;

        // A deadzone before this counts as lost variation: an 8-bit normal map does not
        // reconstruct to exactly unit length even at mip 0, and treating that quantisation
        // as detail would roughen every normal-mapped surface at every distance.
        const float tnLength = saturate(length(tn));
        const float kQuantisationSlack = 0.04;
        const float shortfall = max(0.0, 1.0 - tnLength - kQuantisationSlack);
        normalMapVariance = shortfall / max(tnLength, 1e-3);

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
        // Screen-space derivatives only see the change ACROSS a pixel. Detail finer than a
        // pixel is gone before this runs - the mip chain already averaged it away - so the
        // normal map's own shortfall is added alongside, scaled by the same influence the
        // map was blended in with.
        const float mipVariance = normalMapVariance * 0.60;
        // Dialable, because this widens roughness on every normal-mapped surface in the
        // scene and how much sparkle is worth trading for how much gloss is a look decision.
        const float filterStrength = saturate(fc->shadingParams.x);
        const float kernelRoughness = min(2.0 * variance + mipVariance, kKappa) * filterStrength;
        roughness = sqrt(saturate(roughness * roughness + kernelRoughness));
    }

    // ── Occlusion ─────────────────────────────────────────────────────────
    const float materialAo = MaterialOcclusion(mat, uv);
    const float gtaoVisibility = GtaoVisibility(fc, input.svPosition.xy);
    float ao = saturate(materialAo * gtaoVisibility);

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

    // Same multi-scatter deficit the ambient specular had, and the same terms fix it.
    // Leaving one path compensated and the other not would give a rough metal correct
    // ambient and a highlight half as bright as it should be, which is a worse state
    // than having both wrong together.
    //
    // Ess is the single-scatter directional albedo - the fraction of light the BRDF
    // manages to send back out - so 1/Ess is exactly the factor that restores what the
    // remaining bounces would have carried, weighted by F0 because those bounces are
    // themselves reflections and a dark material swallows them.
    const float2 directEnvBrdf = EnvBrdfApprox(roughness, NdotV);
    const float Ess = max(directEnvBrdf.x + directEnvBrdf.y, 1e-3);
    specular *= 1.0 + F0 * (1.0 / Ess - 1.0);

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
    // Cloud shadow, from the same layer the sky draws. Combined by min for the same reason
    // the contact term is: either occluder blocking the sun is enough.
    const float cloudShadow = receivesShadows
        ? CloudSunVisibility(input.worldPos, L, fc->shadingParams.y, fc->shadingParams.z, fc->elapsedTime)
        : 1.0f;
    const float sunVisibility = receivesShadows ? min(min(shadow, contact), cloudShadow) : 1.0f;

    // The 0.08 floor exists so a shadowed surface does not read as a black hole, and
    // that is a DIFFUSE concern - it stands in for bounce light the renderer does not
    // compute. Specular is a different thing entirely: it is an image of the light
    // source, and a light that is blocked has no image to reflect. Leaking 8% of it was
    // enough to put a sun glint on a glossy floor inside a wall's shadow, because the
    // lobe on a smooth surface is orders of magnitude brighter than 1 and 8% of that is
    // still blinding.
    //
    // So the floor applies where it was meant to and nowhere else. Shadowed specular
    // goes to zero, which is what a blocked sun does.
    const float diffuseVisibility = lerp(1.0f, max(sunVisibility, 0.08f), fc->shadowParams.z);
    const float specularVisibility = lerp(1.0f, sunVisibility, fc->shadowParams.z);
    float3 Lo = (diffuse * diffuseVisibility + specular * specularVisibility) * (sunColor * (sunIntensity * NdotL));

    // Local lights accumulate separately from the sun so each keeps its own
    // shadow term; neither carries an occlusion factor.
    float3 LoLocal = float3(0.0, 0.0, 0.0);

    // Tiled local lights (point + spot).
    // Blend toward geometric normal so local lights remain visibly radial on
    // the ground while still retaining some normal-map detail.
    const float localNormalMapInfluence = 0.35;
    const float3 Nlocal = normalize(lerp(Ngeom, N, localNormalMapInfluence));
    AccumulateTiledLocalLights(
        pc.tileHeaders,
        pc.tileLightIndices,
        pc.lightData,
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
    const float3 skyDiffuse = SkyGradient(N, sunDir, sunColor, sunIntensity, fc->skyHorizonColor.rgb, fc->skyZenithColor.rgb, fc->skyVoidColor.rgb, fc->skyParams, fc->shadingParams.y, 1.0f);

    // Specular probe along the reflection vector, drifting toward the diffuse sample
    // as the surface roughens - a one-sample stand-in for a prefiltered mip chain.
    const float3 R = reflect(-V, N);
    const float3 skySpecularDir = normalize(lerp(R, N, roughness * roughness));
    const float3 skySpecular = SkySpecularProbe(skySpecularDir, input.worldPos, sunDir, sunColor, sunIntensity,
            fc->skyHorizonColor.rgb, fc->skyZenithColor.rgb, fc->skyVoidColor.rgb, fc->skyParams,
            fc->shadingParams.y, fc->shadingParams.z, fc->elapsedTime, roughness);

    const float2 envBrdf = EnvBrdfApprox(roughness, NdotV);

    // ambientColor stays in as the scene's authored floor - the light that is not the
    // sky at all - so an indoor or stylised scene can still set its own base level.
    const float3 ambientDiffuse = (ambientColor + skyDiffuse) * baseColor.rgb * kD;
    // Multi-scatter compensated, so a rough surface stops losing the light that bounces
    // between its microfacets - see EnvBrdf.slangh.
    const float3 ambientSpecular = skySpecular * EnvSpecularWeight(F0, envBrdf);
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
    const float specularAo = SpecularOcclusion(ambientAo, NdotV, roughness);
    float3 ambient = ambientDiffuse * MultiBounceAo(ambientAo, baseColor.rgb) + ambientSpecular * specularAo;

    float3 finalColor = max(Lo + ambient + emissive, float3(0.0, 0.0, 0.0));

    // Atmospheric perspective. Applied here rather than as a separate screen pass
    // because the world position and the sky terms this needs are already in hand;
    // a fullscreen pass would reconstruct both from depth to reach the same answer.
    //
    // Unless the volumetric pass is running, which solves this very integral by
    // marching so that shadows can carve beams out of it. Applying both would count
    // the same scattering twice.
    const float fogAmount = (fc->skyParams.z > 0.0f) ? 0.0f : HeightFogAmount(
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
            fc->fogParams.z,
            fc->skyParams,
            fc->shadingParams.y);
        finalColor = lerp(finalColor, fogColor, fogAmount);
    }

    // Alpha is this surface's coverage. For opaque geometry that is 1 - it fully hides
    // whatever is behind it, which is what the background compositor needs to know. A
    // blended material instead reports how much it actually covers, which is the same
    // number the blend hardware needs, so the two uses agree rather than competing: a
    // half-transparent surface half-covers the background and half-blends with it.
    const float coverage = ((mat.flags & kFlagAlphaBlend) != 0u) ? saturate(baseColor.a) : 1.0f;
    return float4(finalColor, coverage);
}
