// gltf_mesh.slang - Full PBR forward shader for all standard geometry.
//
// Vertex data is fetched via buffer device address stored in DrawInstanceData.vertexBufferAddr.
// No vertex input bindings - the pipeline uses an empty vertex input state.
//
// Push constants  (24 bytes, matches aether::DrawPushConstants):
//   offset  0 : uint64_t  frameAddr         BDA -> FrameConstantsData
//   offset  8 : uint64_t  instanceDataAddr  BDA -> DrawInstanceData[]
//
// FrameConstantsData  (320 bytes, matches aether::FrameConstants):
//   offset   0 : float4x4  viewProj
//   offset  64 : float4x4  view
//   offset 128 : float4x4  proj
//   offset 192 : uint64_t  materialBufferAddr   BDA -> GpuMaterial[]
//   offset 200 : uint64_t  _pad0
//   offset 208 : float4    sunDirectionIntensity  (xyz=world dir, w=intensity)
//   offset 224 : float4    ambientColor           (rgb=ambient, a=unused)
//   offset 240 : float4    cameraWorldPos         (xyz=camera pos, w=1)
//   offset 256 : float4    sunColor               (rgb=sun light color, a=unused)
//   offset 272 : float4    skyHorizonColor        (rgb=sky horizon color, a=unused)
//   offset 288 : float4    skyZenithColor         (rgb=sky zenith color, a=unused)
//   offset 304 : float4    skyVoidColor           (rgb=sky void color, a=unused)
//
// GpuMaterial  (80 bytes, matches aether::GpuMaterial):
//   offset  0  : float4  baseColorFactor
//   offset 16  : float   metallicFactor
//   offset 20  : float   roughnessFactor
//   offset 24  : float   occlusionStrength
//   offset 28  : float   alphaCutoff
//   offset 32  : float4  emissiveFactor  (xyz = emissive RGB)
//   offset 48  : uint    flags           (bit 0=doubleSided, 1=alphaBlend, 2=alphaMask)
//   offset 52  : uint    albedoSlot
//   offset 56  : uint    normalSlot
//   offset 60  : uint    metallicRoughnessSlot
//   offset 64  : uint    occlusionSlot
//   offset 68  : uint    emissiveSlot


// ── Bindless resources ────────────────────────────────────────────────────
[[vk::binding(0, 0)]] Texture2D    g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

// ── Tiled-lighting buffers (descriptor set 1) ────────────────────────────
[[vk::binding(0, 1)]] StructuredBuffer<float4> g_lightData;
[[vk::binding(1, 1)]] StructuredBuffer<uint2>  g_tileHeaders;
[[vk::binding(2, 1)]] StructuredBuffer<uint>   g_tileLightIndices;

#include "include/FrameConstants.slangh"
#include "include/RenderContracts.slangh"
#include "include/DirectionalShadowCSM.slangh"
#include "include/LocalShadow.slangh"

// ── Shared constants ─────────────────────────────────────────────────────
static const uint  kNoTexture     = 0xFFFFFFFFu;
static const float kPI            = 3.14159265358979323846;
static const uint  kFlagDoubleSided = 1u;
static const uint  kFlagAlphaBlend  = 2u;
static const uint  kFlagAlphaMask   = 4u;

// ── GPU structs (layout must exactly match their C++ counterparts) ────────

struct GpuMaterial
{
    [[vk::offset(0)]]  float4 baseColorFactor;
    [[vk::offset(16)]] float  metallicFactor;
    [[vk::offset(20)]] float  roughnessFactor;
    [[vk::offset(24)]] float  occlusionStrength;
    [[vk::offset(28)]] float  alphaCutoff;
    [[vk::offset(32)]] float4 emissiveFactor;          // xyz = emissive RGB, w unused
    [[vk::offset(48)]] uint   flags;
    [[vk::offset(52)]] uint   albedoSlot;
    [[vk::offset(56)]] uint   normalSlot;
    [[vk::offset(60)]] uint   metallicRoughnessSlot;
    [[vk::offset(64)]] uint   occlusionSlot;
    [[vk::offset(68)]] uint   emissiveSlot;
    [[vk::offset(72)]] uint   _pad0;
    [[vk::offset(76)]] uint   _pad1;
};

[[vk::push_constant]] DrawPushConstants pc;

// ── Vertex layout (must match aether::Mesh::Vertex, 92 bytes) ─────────────

struct Vertex
{
    [[vk::offset(0)]]  float3 position;
    [[vk::offset(12)]] float3 normal;
    [[vk::offset(24)]] float4 tangent;      // w = bitangent sign
    [[vk::offset(40)]] float2 uv;
    [[vk::offset(48)]] float3 color;
    [[vk::offset(60)]] uint4  jointIndices;
    [[vk::offset(76)]] float4 jointWeights;
};

struct VSOutput
{
    float4 svPosition                          : SV_Position;
    [[vk::location(0)]] float3 worldPos        : TEXCOORD0;
    [[vk::location(1)]] float3 worldNormal     : TEXCOORD1;
    [[vk::location(2)]] float3 worldTangent    : TEXCOORD2;
    [[vk::location(3)]] float  tangentSign     : TEXCOORD3;
    [[vk::location(4)]] float2 uv              : TEXCOORD4;
    [[vk::location(5)]] float3 vertexColor     : TEXCOORD5;
    [[vk::location(6)]] nointerpolation uint materialIndex : TEXCOORD6;
};

// Transform direction vectors (normal/tangent) through a world matrix basis,
// correcting for non-uniform axis scale without requiring matrix inverse().
// basis must be the TRANSPOSED float3x3 of the model matrix so that basis[i]
// is the i-th column of the model matrix (= world-space basis vector i).
float3 TransformDirectionScaleCorrect(float3 v, float3x3 basis)
{
    float3 c0 = basis[0];
    float3 c1 = basis[1];
    float3 c2 = basis[2];

    const float d0 = max(dot(c0, c0), 1e-8);
    const float d1 = max(dot(c1, c1), 1e-8);
    const float d2 = max(dot(c2, c2), 1e-8);

    return normalize(c0 * (v.x / d0) + c1 * (v.y / d1) + c2 * (v.z / d2));
}

// ── Vertex shader ─────────────────────────────────────────────────────────

// SV_StartInstanceLocation == gl_BaseInstance == firstInstance from the draw command.
// All draws use instanceCount=1, so this directly selects the right DrawInstanceData slot.
// Do NOT add SV_InstanceID: Slang maps it to gl_InstanceIndex which already includes
// the base, so adding both would double-count firstInstance and read the wrong slot.
[shader("vertex")]
VSOutput vertexMain(uint vertexId : SV_VertexID, uint startInstance : SV_StartInstanceLocation)
{
    FrameConstantsData* fc = (FrameConstantsData*)(pc.frameAddr);
    DrawInstanceData* instances = (DrawInstanceData*)(pc.instanceDataAddr);
    const uint instanceIndex = startInstance;
    DrawInstanceData instance = instances[instanceIndex];

    Vertex* verts = (Vertex*)(instance.vertexBufferAddr);
    Vertex input = verts[vertexId];

    float4 worldPos;
    float3 worldNormal;
    float3 worldTangent;

    if (pc.skinPaletteAddr != 0uL && instance.skinJointCount > 0u)
    {
        // GPU skinning: blend up to 4 joint matrices weighted by BLENDWEIGHT0.
        float4x4* joints = (float4x4*)(pc.skinPaletteAddr);
        const uint base = instance.skinPaletteOffset;
        float4x4 skinMatrix =
            input.jointWeights.x * joints[base + input.jointIndices.x] +
            input.jointWeights.y * joints[base + input.jointIndices.y] +
            input.jointWeights.z * joints[base + input.jointIndices.z] +
            input.jointWeights.w * joints[base + input.jointIndices.w];

        float4x4 worldMatrix = mul(instance.model, skinMatrix);
        worldPos = mul(worldMatrix, float4(input.position, 1.0));

        // Scale-correct normal/tangent transform for non-uniform scaling.
        // float3x3(row0, row1, row2) gives HLSL rows; transpose so that basis[i]
        // is column i of the world matrix, as TransformDirectionScaleCorrect expects.
        float3x3 world3 = transpose(float3x3(
            worldMatrix[0].xyz,
            worldMatrix[1].xyz,
            worldMatrix[2].xyz
        ));
        worldNormal  = TransformDirectionScaleCorrect(input.normal, world3);
        worldTangent = TransformDirectionScaleCorrect(input.tangent.xyz, world3);
    }
    else
    {
        worldPos     = mul(instance.model, float4(input.position, 1.0));

        // Same scale-correct path for static meshes.
        float3x3 model3 = transpose(float3x3(
            instance.model[0].xyz,
            instance.model[1].xyz,
            instance.model[2].xyz
        ));
        worldNormal  = TransformDirectionScaleCorrect(input.normal, model3);
        worldTangent = TransformDirectionScaleCorrect(input.tangent.xyz, model3);
    }

    VSOutput o;
    o.svPosition  = mul(fc->viewProj, worldPos);
    o.worldPos    = worldPos.xyz;
    o.worldNormal = worldNormal;
    o.worldTangent = worldTangent;
    o.tangentSign = input.tangent.w;
    o.uv          = input.uv;
    o.vertexColor = input.color;
    o.materialIndex = instance.materialIndex;
    return o;
}

// ── PBR utilities ─────────────────────────────────────────────────────────

// GGX / Trowbridge-Reitz normal distribution.
float D_GGX(float NdotH, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / max(kPI * d * d, 1e-7);
}

// Smith masking-shadowing (Schlick-GGX).
float G_SmithSchlick(float NdotV, float NdotL, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) * 0.125;    // k = (r+1)^2/8 for direct lighting
    float gv = NdotV / max(NdotV * (1.0 - k) + k, 1e-7);
    float gl = NdotL / max(NdotL * (1.0 - k) + k, 1e-7);
    return gv * gl;
}

// Fresnel-Schlick approximation.
float3 F_Schlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

struct LocalLight
{
    float3 position;
    float  radius;
    float3 color;
    float  intensity;
    float3 direction;
    uint   type; // 0=point, 1=spot
    float  innerCos;
    float  outerCos;
    float  shadowIndex;  // >= 0 = index into shadowLightData[]
    float  shadowStrength;
};

LocalLight LoadLocalLight(uint index)
{
    // CPU packs 5 float4 rows per light (4th row = shadow data).
    uint base = index * 5u;
    float4 p0 = g_lightData[base + 0u];
    float4 p1 = g_lightData[base + 1u];
    float4 p2 = g_lightData[base + 2u];
    float4 p3 = g_lightData[base + 3u];
    float4 p4 = g_lightData[base + 4u];

    LocalLight l;
    l.position = p0.xyz;
    l.radius = max(p0.w, 1e-4);
    l.color = p1.xyz;
    l.intensity = p1.w;
    l.direction = normalize(p2.xyz);
    l.type = (uint)round(p2.w);
    l.innerCos = p3.x;
    l.outerCos = p3.y;
    l.shadowIndex = p4.x;
    l.shadowStrength = p4.y;
    return l;
}

void AccumulateLocalLight(
    in LocalLight light,
    in float3 worldPos,
    in float3 N,
    in float3 V,
    in float3 F0,
    in float3 baseColor,
    in float metallic,
    in float roughness,
    in FrameConstantsData* fc,
    inout float3 sum)
{
    float3 toLight = light.position - worldPos;
    float dist2 = dot(toLight, toLight);
    float dist = sqrt(max(dist2, 1e-8));
    float3 L = toLight / max(dist, 1e-4);

    float rangeAtten = saturate(1.0 - (dist / light.radius));
    rangeAtten *= rangeAtten;
    if (rangeAtten <= 1e-5)
    {
        return;
    }

    float spotAtten = 1.0;
    if (light.type == 1u)
    {
        // Spotlight cone is defined by light.direction (forward axis).
        float cd = dot(normalize(light.direction), -L);
        float den = max(light.innerCos - light.outerCos, 1e-5);
        spotAtten = saturate((cd - light.outerCos) / den);
        if (spotAtten <= 1e-5)
        {
            return;
        }
    }

    float3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    if (NdotL <= 1e-5)
    {
        return;
    }
    NdotV = max(NdotV, 1e-4);

    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    float D = D_GGX(NdotH, roughness);
    float G = G_SmithSchlick(NdotV, NdotL, roughness);
    float3 F = F_Schlick(VdotH, F0);
    float3 specular = (D * G * F) / max(4.0 * NdotL * NdotV, 1e-4);

    float3 kD = (float3(1.0, 1.0, 1.0) - F) * (1.0 - metallic);
    float3 diffuse = kD * baseColor / kPI;

    float3 radiance = light.color * (light.intensity * rangeAtten * spotAtten);

    // Keep local highlights, but bias toward diffuse so point emitters do not
    // read like directional "torch" beams on highly detailed normal maps.
    const float localSpecularWeight = 0.28;
    float shadowVis = 1.0;

    // Evaluate local shadow if this light has an atlas entry.
    if (light.shadowIndex >= 0.0 && fc->shadowAtlasSlot != 0xFFFFFFFFu)
    {
        uint shadowIdx = (uint)light.shadowIndex;
        shadowVis = SampleLocalShadow(shadowIdx, fc->shadowAtlasSlot, worldPos, g_linearSampler, fc);
        shadowVis = lerp(1.0, shadowVis, light.shadowStrength);
    }

    sum += (diffuse + specular * localSpecularWeight) * radiance * NdotL * shadowVis;
}

// ── Fragment shader ───────────────────────────────────────────────────────

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    FrameConstantsData* fc        = (FrameConstantsData*)(pc.frameAddr);
    GpuMaterial*        materials = (GpuMaterial*)(fc->materialBufferAddr);

    // Safe fallback when a draw is missing a valid registered material.
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

    if (input.materialIndex != kNoTexture)
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
    float3 vertexColor = input.vertexColor;
    if (dot(vertexColor, vertexColor) < 1e-8)
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
        // Fallback for assets that came in with degenerate/missing normals.
        float3 faceN = cross(ddx(input.worldPos), ddy(input.worldPos));
        const float faceLen2 = dot(faceN, faceN);
        N = (faceLen2 > 1e-12) ? normalize(faceN) : float3(0.0, 1.0, 0.0);
    }

    float3 Traw = input.worldTangent;
    const float tRawLen2 = dot(Traw, Traw);
    float3 T = (tRawLen2 > 1e-12) ? normalize(Traw) : float3(1.0, 0.0, 0.0);
    T = T - N * dot(T, N);                      // Gram-Schmidt re-orthogonalise
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

        uint2 header = g_tileHeaders[tile];
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
            uint lightIndex = g_tileLightIndices[start + i];
            LocalLight light = LoadLocalLight(lightIndex);
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

    float3 finalColor = Lo + ambient + emissive;

    return float4(finalColor, baseColor.a);
}
