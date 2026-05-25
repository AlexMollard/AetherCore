// vsm_blur.slang - Separable Gaussian blur for VSM shadow atlas.
// Single entry point; dispatch twice (first horizontal, then vertical).

[[vk::binding(0, 0)]] RWTexture2D<float2> g_atlas;
[[vk::binding(1, 0)]] Texture2D<float2>  g_input;

struct BlurPushConstants
{
    uint2  atlasSize;   // width, height
    uint   isHorizontal; // 0 = vertical, 1 = horizontal
    float  _pad0;
};

[[vk::push_constant]] BlurPushConstants pc;

// 5-tap Gaussian weights.
static const float kWeights[5] = { 0.153388, 0.221461, 0.250301, 0.221461, 0.153388 };
static const float kOffsets[5] = { -2.0, -1.0, 0.0, 1.0, 2.0 };

groupshared float2 s_Shared[256];

// Each 16x16 thread group processes a 16-wide strip of the atlas.
[numthreads(256, 1, 1)]
[shader("compute")]
void main(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    uint row = gid.y;
    uint colBase = gid.x * 256u;

    uint2 pos;
    if (pc.isHorizontal != 0u)
    {
        pos = uint2(colBase + gtid.x, row);
    }
    else
    {
        pos = uint2(row, colBase + gtid.x);
    }

    if (pos.x >= pc.atlasSize.x || pos.y >= pc.atlasSize.y)
    {
        return;
    }

    s_Shared[gtid.x] = g_input[pos];

    GroupMemoryBarrierWithGroupSync();

    float2 result = float2(0.0, 0.0);
    [unroll]
    for (int t = 0; t < 5; ++t)
    {
        int sharedIdx = int(gtid.x) + int(kOffsets[t]);
        if (sharedIdx >= 0 && sharedIdx < 256)
        {
            result += s_Shared[sharedIdx] * kWeights[t];
        }
    }

    g_atlas[pos] = result;
}
