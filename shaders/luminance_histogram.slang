// Compute histogram of log2(luminance) for both the HDR (pre-tonemap) and LDR
// (post-tonemap) color buffers. Output is a pair of 256-bin uint histograms
// written to a mapped GpuToCpu buffer via BDA.
//
// Single workgroup (256 threads) processes every pixel by striding the pixel
// index by 256. Groupshared counters avoid global-memory atomics; the final
// accumulation is written in one coherent store per thread.

#include "include/DevicePointer.slangh"
#include "include/BindlessContract.slangh"

MEOW_BINDLESS_BINDING
Texture2D g_textures[];

struct HistogramPush
{
    uint        hdrSlot;
    uint        ldrSlot;
    DevicePtr<uint> output; // 512 uints: [0..255] HDR, [256..511] LDR
    uint        width;
    uint        height;
};
[[vk::push_constant]] HistogramPush pc;

static const float kLogMin   = -20.0;
static const float kLogRange =  30.0;
static const float kEpsilon  =  1e-6;

groupshared uint g_hdrHist[256];
groupshared uint g_ldrHist[256];

uint LuminanceBin(float3 color)
{
    float lum = dot(color, float3(0.2126, 0.7152, 0.0722));
    float logLum = log2(max(lum, kEpsilon));
    return uint(clamp((logLum - kLogMin) * (255.0 / kLogRange), 0.0, 255.0));
}

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint3 gtid : SV_GroupThreadID)
{
    uint idx = gtid.x;

    g_hdrHist[idx] = 0;
    g_ldrHist[idx] = 0;
    GroupMemoryBarrierWithGroupSync();

    uint totalPixels = pc.width * pc.height;

    for (uint i = idx; i < totalPixels; i += 256)
    {
        uint x = i % pc.width;
        uint y = i / pc.width;

        float3 hdr = g_textures[pc.hdrSlot].Load(int3(x, y, 0)).rgb;
        InterlockedAdd(g_hdrHist[LuminanceBin(hdr)], 1);

        float3 ldr = g_textures[pc.ldrSlot].Load(int3(x, y, 0)).rgb;
        InterlockedAdd(g_ldrHist[LuminanceBin(ldr)], 1);
    }

    GroupMemoryBarrierWithGroupSync();

    pc.output.Store(idx,       g_hdrHist[idx]);
    pc.output.Store(idx + 256, g_ldrHist[idx]);
}
