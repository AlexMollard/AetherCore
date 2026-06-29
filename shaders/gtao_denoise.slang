// Half-resolution bilateral blur for GTAO.

[[vk::binding(0, 0)]] Texture2D g_textures[];

#include "include/FullScreenTriangle.slangh"

struct DenoisePush
{
    uint depthSlot;
    uint srcAoSlot;
    uint fullWidth;
    uint fullHeight;
    uint aoWidth;
    uint aoHeight;
    float edgeThreshold;
    float spatialSigma;
};

[[vk::push_constant]] DenoisePush pc;

static uint2 ClampAo(int2 p)
{
    return uint2(
        uint(clamp(p.x, 0, int(pc.aoWidth) - 1)),
        uint(clamp(p.y, 0, int(pc.aoHeight) - 1)));
}

static uint2 ClampFull(int2 p)
{
    return uint2(
        uint(clamp(p.x, 0, int(pc.fullWidth) - 1)),
        uint(clamp(p.y, 0, int(pc.fullHeight) - 1)));
}

static float LoadDepthForHalf(uint2 halfPx)
{
    return g_textures[pc.depthSlot].Load(int3(ClampFull(int2(halfPx * 2u)), 0)).r;
}

static float LoadAO(uint2 halfPx)
{
    return g_textures[pc.srcAoSlot].Load(int3(halfPx, 0)).r;
}

[shader("fragment")]
float4 fragmentMain(float4 svPos : SV_Position) : SV_Target0
{
    if (pc.aoWidth == 0u || pc.aoHeight == 0u || pc.fullWidth == 0u || pc.fullHeight == 0u)
    {
        return float4(1.0f, 0.0f, 0.0f, 1.0f);
    }

    uint2 center = ClampAo(int2(svPos.xy));
    float centerDepth = LoadDepthForHalf(center);
    if (centerDepth >= 1.0f)
    {
        return float4(1.0f, 0.0f, 0.0f, 1.0f);
    }

    float sum = 0.0f;
    float weightSum = 0.0f;
    const int radius = 2;

    [unroll]
    for (int y = -radius; y <= radius; ++y)
    {
        [unroll]
        for (int x = -radius; x <= radius; ++x)
        {
            uint2 p = ClampAo(int2(center) + int2(x, y));
            float depth = LoadDepthForHalf(p);
            float2 ofs = float2(float(x), float(y));
            float spatial = exp(-dot(ofs, ofs) / max(2.0f * pc.spatialSigma * pc.spatialSigma, 1e-3f));
            float depthDiff = abs(depth - centerDepth);
            float range = exp(-(depthDiff * depthDiff) / max(pc.edgeThreshold, 1e-6f));
            float weight = spatial * range;
            sum += LoadAO(p) * weight;
            weightSum += weight;
        }
    }

    return float4(sum / max(weightSum, 1e-4f), 0.0f, 0.0f, 1.0f);
}
