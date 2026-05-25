// vsm_blur.slang - Separable Gaussian blur for VSM shadow atlas.
// Single entry point; dispatch twice (first horizontal, then vertical).

[[vk::binding(0, 0)]] RWTexture2D<float2> g_output;
[[vk::binding(1, 0)]] Texture2D<float2>  g_input;

struct BlurPushConstants
{
    uint2  atlasSize;
    uint   isHorizontal;
    float  _pad0;
};

[[vk::push_constant]] BlurPushConstants pc;

// 5-tap Gaussian weights.
static const float kWeights[5] = { 0.153388, 0.221461, 0.250301, 0.221461, 0.153388 };
static const int kOffsets[5] = { -2, -1, 0, 1, 2 };

[numthreads(16, 16, 1)]
[shader("compute")]
void main(uint3 gid : SV_DispatchThreadID)
{
    if (gid.x >= pc.atlasSize.x || gid.y >= pc.atlasSize.y)
    {
        return;
    }

    float2 result = float2(0.0, 0.0);

    [unroll]
    for (int t = 0; t < 5; ++t)
    {
        uint2 pos = gid.xy;
        if (pc.isHorizontal != 0u)
        {
            pos.x = uint(max(min(int(gid.x) + kOffsets[t], int(pc.atlasSize.x) - 1), 0));
        }
        else
        {
            pos.y = uint(max(min(int(gid.y) + kOffsets[t], int(pc.atlasSize.y) - 1), 0));
        }
        result += g_input[pos] * kWeights[t];
    }

    g_output[gid.xy] = result;
}
