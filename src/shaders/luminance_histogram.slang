// Compute luminance histograms for the HDR (pre-tonemap) and LDR
// (post-tonemap) color buffers. Output is a pair of 256-bin uint histograms
// written to a mapped GpuToCpu buffer via BDA. HDR uses log2 luminance for
// exposure metering and debugging; LDR uses linear 0..1 luminance because the
// tonemapped target is already display-range.
//
// The HDR histogram drives auto-exposure, so what it measures has to depend on
// the scene and nothing else. Two properties make that true:
//
//   * A FIXED sample grid. The grid is a set number of samples spread across
//     the image in UV, not a stride in pixels, so each sample always covers the
//     same fraction of the screen no matter what the render resolution is.
//   * Each sample AVERAGES its footprint instead of point-sampling one pixel
//     inside it. This is the part that matters: metering averages log2
//     luminance, and the log of an average is not the average of the logs, so a
//     noisy region meters darker than a smooth one of the same brightness. An
//     aliased surface gets smoother as the render resolution drops, so point
//     sampling made the metered luminance - and with it the whole image's
//     exposure and saturation - a function of the resolution. Averaging the
//     footprint first removes that: the tap count per axis is derived from how
//     many source texels the footprint spans, so the same solid angle is
//     integrated either way.
//
// The host clears the output buffer, then this shader runs many workgroups over
// the grid. Each workgroup builds local groupshared bins and only contributes
// nonzero bins to the global output. Keep the log range in sync with
// PostProcessStack::kHistogramLogMin/kHistogramLogMax.

#include "include/DevicePointer.slangh"

// Bindless sampled images at set 0 (no per-draw set in this pass), matching
// the engine-wide descriptor heap mapping (see BindlessManager).
[[vk::binding(0, 0)]]
Texture2D g_textures[];
[[vk::binding(1, 0)]]
SamplerState g_linearSampler;

struct HistogramPush
{
    uint        hdrSlot;
    uint        ldrSlot;
    DevicePtr<uint> output; // 512 uints: [0..255] HDR, [256..511] LDR
    uint        width;      // source pixels, used to size the footprint taps
    uint        height;
    uint        meterWidth; // sample grid, fixed regardless of resolution
    uint        meterHeight;
};
[[vk::push_constant]] HistogramPush pc;

static const float kLogMin   = -10.0;
static const float kLogRange =  20.0;
static const float kEpsilon  =  1e-6;
static const uint  kThreadCount = 256;

// One bilinear tap already averages a 2x2 texel neighbourhood, so two taps per
// axis integrate sixteen source texels per sample. Covering the footprint
// completely would mean reading every pixel of both buffers every time the
// histogram runs, which costs far more than metering is worth; sixteen texels
// already cut the variance - and with it the resolution sensitivity this whole
// scheme exists to remove - to well under a percent.
static const uint kMaxTapsPerAxis = 2u;

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

// The global sampler addresses REPEAT; clamping keeps a tap on the edge of the
// image instead of wrapping to the far side of it.
float3 SampleRGB(uint slot, float2 uv)
{
    const float2 halfTexel = 0.5 / float2(max(pc.width, 1u), max(pc.height, 1u));
    return g_textures[slot].SampleLevel(g_linearSampler, clamp(uv, halfTexel, 1.0 - halfTexel), 0.0).rgb;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    uint idx = gtid.x;

    g_hdrHist[idx] = 0;
    g_ldrHist[idx] = 0;
    GroupMemoryBarrierWithGroupSync();

    uint meterWidth  = max(pc.meterWidth, 1u);
    uint meterHeight = max(pc.meterHeight, 1u);
    uint sampleIndex = gid.x * kThreadCount + idx;
    uint totalSamples = meterWidth * meterHeight;

    if (sampleIndex < totalSamples)
    {
        float2 rcpMeter = 1.0 / float2(meterWidth, meterHeight);
        float2 uvMin = float2(sampleIndex % meterWidth, sampleIndex / meterWidth) * rcpMeter;

        // How many source texels this sample stands for, and therefore how many
        // taps it takes to integrate them.
        float2 footprint = float2(pc.width, pc.height) * rcpMeter;
        uint2  taps = clamp(uint2(ceil(footprint * 0.5)), 1u, kMaxTapsPerAxis);
        float2 tapStep = rcpMeter / float2(taps);

        float3 hdrSum = 0.0;
        float3 ldrSum = 0.0;
        for (uint ty = 0u; ty < taps.y; ++ty)
        {
            for (uint tx = 0u; tx < taps.x; ++tx)
            {
                float2 uv = uvMin + (float2(tx, ty) + 0.5) * tapStep;
                hdrSum += SampleRGB(pc.hdrSlot, uv);
                ldrSum += SampleRGB(pc.ldrSlot, uv);
            }
        }

        float invTaps = 1.0 / float(taps.x * taps.y);
        InterlockedAdd(g_hdrHist[HdrLuminanceBin(hdrSum * invTaps)], 1);
        InterlockedAdd(g_ldrHist[LdrLuminanceBin(ldrSum * invTaps)], 1);
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
