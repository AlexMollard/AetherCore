// Half-resolution ground-truth ambient occlusion.
// Writes one R8 value per half-res pixel; depth is sampled from the predepth pass.

[[vk::binding(0, 0)]] Texture2D g_textures[];

#include "include/FrameConstants.slangh"
#include "include/FullScreenTriangle.slangh"

struct GTAOPush
{
    uint fullWidth;
    uint fullHeight;
    uint frameIndex;
    uint64_t frameConstantsAddr;
    float radius;
    float strength;
    uint _pad0;
    uint _pad1;
};

[[vk::push_constant]] GTAOPush pc;

static float Hash(uint s)
{
    s = s * 747796405u + 2891336453u;
    s = ((s >> 16u) ^ s) * 0x45d9f3bu;
    s = ((s >> 16u) ^ s) * 0x45d9f3bu;
    s = (s >> 16u) ^ s;
    return float(s) * (1.0f / 4294967296.0f);
}

static float2 Hash2(uint2 px)
{
    return float2(Hash(px.x + px.y * 65537u), Hash(px.x * 65537u + px.y + 17u));
}

static uint2 ClampFull(int2 p)
{
    return uint2(
        uint(clamp(p.x, 0, int(pc.fullWidth) - 1)),
        uint(clamp(p.y, 0, int(pc.fullHeight) - 1)));
}

static float LoadDepth(uint2 px, FrameConstantsData* fc)
{
    ResourceEntry depthEntry = LoadResourceEntry(fc->resourceTableAddr, kFrameResourceId_SceneDepth);
    if (depthEntry.type != kResourceTypeBindlessTexture || depthEntry.address == 0xFFFFFFFFu || depthEntry.width == 0u || depthEntry.height == 0u)
    {
        return 1.0f;
    }
    return g_textures[uint(depthEntry.address)].Load(int3(px, 0)).r;
}

static float3 ReconstructWorld(uint2 px, float depth, FrameConstantsData* fc)
{
    float2 uv = (float2(px) + 0.5f) / float2(pc.fullWidth, pc.fullHeight);
    float2 ndc = uv * 2.0f - 1.0f;
    float4 clipPos = float4(ndc, depth, 1.0f);
    float4 worldPos = mul(fc->invViewProj, clipPos);
    return worldPos.xyz / max(worldPos.w, 1e-6f);
}

static float3 NormalFromDepth(uint2 px, float centerDepth, FrameConstantsData* fc)
{
    uint2 l = ClampFull(int2(px) + int2(-1, 0));
    uint2 r = ClampFull(int2(px) + int2(1, 0));
    uint2 u = ClampFull(int2(px) + int2(0, -1));
    uint2 d = ClampFull(int2(px) + int2(0, 1));

    float3 wl = ReconstructWorld(l, LoadDepth(l, fc), fc);
    float3 wr = ReconstructWorld(r, LoadDepth(r, fc), fc);
    float3 wu = ReconstructWorld(u, LoadDepth(u, fc), fc);
    float3 wd = ReconstructWorld(d, LoadDepth(d, fc), fc);

    float3 n = normalize(cross(wd - wu, wr - wl));
    return (dot(n, n) > 0.0f) ? n : float3(0.0f, 0.0f, 1.0f);
}

static float ComputeAO(uint2 px, float3 worldPos, float3 normal, FrameConstantsData* fc)
{
    static const uint kDirectionCount = 6u;
    static const uint kStepCount = 5u;

    float2 noise = Hash2(px);
    float3 viewDir = normalize(fc->cameraWorldPos.xyz - worldPos);
    float3 tangent = cross(normal, abs(viewDir.y) < 0.9f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f));
    tangent = normalize(tangent);
    float3 bitangent = normalize(cross(normal, tangent));

    float occlusion = 0.0f;
    float weightSum = 0.0f;

    [unroll]
    for (uint d = 0u; d < kDirectionCount; ++d)
    {
        float angle = (float(d) + noise.x) * (6.2831853f / float(kDirectionCount));
        float s;
        float c;
        sincos(angle, s, c);
        float3 dir = normalize(c * tangent + s * bitangent + normal * 0.35f);

        [unroll]
        for (uint step = 1u; step <= kStepCount; ++step)
        {
            float t = (float(step) + noise.y) / float(kStepCount);
            float3 sampleWorld = worldPos + dir * (pc.radius * t);
            float4 sampleClip = mul(fc->viewProj, float4(sampleWorld, 1.0f));
            if (sampleClip.w <= 1e-5f)
            {
                continue;
            }

            float3 sampleNdc = sampleClip.xyz / sampleClip.w;
            float2 sampleUv = sampleNdc.xy * 0.5f + 0.5f;
            if (any(sampleUv < 0.0f) || any(sampleUv > 1.0f))
            {
                continue;
            }

            uint2 samplePx = ClampFull(int2(sampleUv * float2(pc.fullWidth, pc.fullHeight)));
            float sampleDepth = LoadDepth(samplePx, fc);
            if (sampleDepth >= 1.0f)
            {
                continue;
            }

            float3 depthWorld = ReconstructWorld(samplePx, sampleDepth, fc);
            float3 delta = depthWorld - worldPos;
            float dist = length(delta);
            float rangeWeight = saturate(1.0f - (dist / max(pc.radius, 1e-3f)));
            float horizon = saturate(dot(normalize(delta), normal) * -2.0f);
            occlusion += horizon * rangeWeight;
            weightSum += rangeWeight;
        }
    }

    float ao = 1.0f - occlusion / max(weightSum, 1e-3f);
    return max(pow(saturate(ao), pc.strength), 0.35f);
}

[shader("fragment")]
float4 fragmentMain(float4 svPos : SV_Position) : SV_Target0
{
    uint2 halfPx = uint2(svPos.xy);
    FrameConstantsData* fc = (FrameConstantsData*)pc.frameConstantsAddr;

    if (pc.fullWidth == 0u || pc.fullHeight == 0u)
    {
        return float4(1.0f, 0.0f, 0.0f, 1.0f);
    }

    uint2 fullPx = ClampFull(int2(halfPx * 2u));
    float depth = LoadDepth(fullPx, fc);
    if (depth >= 1.0f)
    {
        return float4(1.0f, 0.0f, 0.0f, 1.0f);
    }

    float3 worldPos = ReconstructWorld(fullPx, depth, fc);
    float3 normal = NormalFromDepth(fullPx, depth, fc);
    float ao = ComputeAO(fullPx, worldPos, normal, fc);
    return float4(ao, 0.0f, 0.0f, 1.0f);
}
