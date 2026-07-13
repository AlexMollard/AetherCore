// Compute luminance histograms for the HDR (pre-tonemap) and LDR
// (post-tonemap) color buffers. Output is a pair of 256-bin uint histograms
// written to a mapped GpuToCpu buffer via BDA. HDR uses log2 luminance for
// exposure debugging; LDR uses linear 0..1 luminance because the tonemapped
// target is already display-range.
//
// The host clears the output buffer, then this shader runs many workgroups over
// a downsampled pixel grid. Each workgroup builds local groupshared bins and
// only contributes nonzero bins to the global output. Keep the log range in
// sync with PostProcessStack::kHistogramLogMin/kHistogramLogMax.

#include "include/DevicePointer.slangh"

// Bindless sampled images at set 0 (no per-draw set in this pass), matching
// the engine-wide descriptor heap mapping (see BindlessManager).
[[vk::binding(0, 0)]]
Texture2D g_textures[];

struct HistogramPush
{
    uint        hdrSlot;
    uint        ldrSlot;
    DevicePtr<uint> output; // 512 uints: [0..255] HDR, [256..511] LDR
    uint        width;
    uint        height;
    uint        sampleStride;
};
[[vk::push_constant]] HistogramPush pc;

static const float kLogMin   = -10.0;
static const float kLogRange =  20.0;
static const float kEpsilon  =  1e-6;
static const uint  kThreadCount = 256;

groupshared uint g_hdrHist[256];
groupshared uint g_ldrHist[256];

float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

uint HdrLuminanceBin(float3 color)
{
    float lum = Luminance(color);
    float logLum = log2(max(lum, kEpsilon));
    return uint(clamp((logLum - kLogMin) * (255.0 / kLogRange), 0.0, 255.0));
}

uint LdrLuminanceBin(float3 color)
{
    return uint(clamp(Luminance(color), 0.0, 1.0) * 255.0);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    uint idx = gtid.x;

    g_hdrHist[idx] = 0;
    g_ldrHist[idx] = 0;
    GroupMemoryBarrierWithGroupSync();

    uint stride = max(pc.sampleStride, 1);
    uint sampleWidth = (pc.width + stride - 1) / stride;
    uint sampleHeight = (pc.height + stride - 1) / stride;
    uint sampleIndex = gid.x * kThreadCount + idx;
    uint totalSamples = sampleWidth * sampleHeight;

    if (sampleIndex < totalSamples)
    {
        uint sampleX = sampleIndex % sampleWidth;
        uint sampleY = sampleIndex / sampleWidth;
        uint x = min(sampleX * stride, pc.width - 1);
        uint y = min(sampleY * stride, pc.height - 1);

        float3 hdr = g_textures[pc.hdrSlot].Load(int3(x, y, 0)).rgb;
        InterlockedAdd(g_hdrHist[HdrLuminanceBin(hdr)], 1);

        float3 ldr = g_textures[pc.ldrSlot].Load(int3(x, y, 0)).rgb;
        InterlockedAdd(g_ldrHist[LdrLuminanceBin(ldr)], 1);
    }

    GroupMemoryBarrierWithGroupSync();

    uint hdrCount = g_hdrHist[idx];
    if (hdrCount != 0)
    {
        InterlockedAdd(pc.output.At(idx)[0], hdrCount);
    }

    uint ldrCount = g_ldrHist[idx];
    if (ldrCount != 0)
    {
        InterlockedAdd(pc.output.At(idx + 256)[0], ldrCount);
    }
}
