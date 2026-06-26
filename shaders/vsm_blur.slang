// Separable Gaussian blur for VSM shadow atlas.
// Single entry point; dispatch twice (first horizontal, then vertical).
//
// Uses groupshared memory to cache the 5-tap filter window:
// each 16x16 thread group loads a 20x20 tile (2-texel border for kernel radius)
// into LDS, then reads from LDS for the convolution. This replaces 5 global
// buffer reads per thread with 1 global write + barrier + 5 LDS reads.
//
// Input/output via DevicePtr<float2> (BDA) stored in push constants -
// zero descriptor bindings required.

#include "include/DevicePointer.slangh"

struct BlurPushConstants
{
    [[vk::offset(0)]]  uint2  atlasSize;
    [[vk::offset(8)]]  uint2  blurOffset;  // Top-left corner of the region to blur
    [[vk::offset(16)]] uint   isHorizontal;
    [[vk::offset(20)]] float  _pad0;
    [[vk::offset(24)]] float  _pad1;
    [[vk::offset(28)]] float  _pad2;
    [[vk::offset(32)]] DevicePtr<float2> srcData;
    [[vk::offset(40)]] DevicePtr<float2> dstData;
};

[[vk::push_constant]] BlurPushConstants pc;

// 5-tap Gaussian weights.
static const float kWeights[5] = { 0.153388, 0.221461, 0.250301, 0.221461, 0.153388 };
static const int kOffsets[5] = { -2, -1, 0, 1, 2 };

// Shared tile: 20x20 float2 (3200 bytes) covers the 16x16 output region
// plus a 2-texel border on each side for the [-2, +2] kernel radius.
groupshared float2 tile[20][20];

uint2 ClampCoord(int2 p, uint2 atlasSize)
{
    return uint2(
        uint(clamp(p.x, 0, int(atlasSize.x) - 1)),
        uint(clamp(p.y, 0, int(atlasSize.y) - 1))
    );
}

float2 LoadInput(uint2 coord, uint2 atlasSize, DevicePtr<float2> src)
{
    return src.Load(coord.y * atlasSize.x + coord.x);
}

void StoreOutput(uint2 coord, uint2 atlasSize, float2 value, DevicePtr<float2> dst)
{
    dst.Store(coord.y * atlasSize.x + coord.x, value);
}

[numthreads(16, 16, 1)]
[shader("compute")]
void main(uint3 dtid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID)
{
    if (pc.srcData.addr == 0 || pc.dstData.addr == 0 || pc.atlasSize.x == 0 || pc.atlasSize.y == 0)
    {
        return;
    }

    // Workgroup origin (top-left of this 16x16 output tile) + blur offset.
    uint2 origin = dtid.xy - gtid.xy + pc.blurOffset;
    uint2 globalPos = dtid.xy + pc.blurOffset;

    // ── Phase 1: Load padded 20x20 tile into LDS ──

    // Interior: all 256 threads load the core 16x16 region.
    if (pc.isHorizontal != 0u)
    {
        // ── Horizontal pass ─────────────────────────────────────────────
        // Interior maps to tile[ty][tx + 2], covering output columns [0, 15].
        int2 base = int2(origin) + int2(gtid.xy);
        tile[gtid.y][gtid.x + 2] = LoadInput(ClampCoord(base, pc.atlasSize), pc.atlasSize, pc.srcData);

        // Left border: threads with tx < 2 load tile[ty][0..1].
        if (gtid.x < 2)
        {
            int2 p = int2(origin) + int2(gtid.xy) - int2(2, 0);
            tile[gtid.y][gtid.x] = LoadInput(ClampCoord(p, pc.atlasSize), pc.atlasSize, pc.srcData);
        }

        // Right border: threads with tx >= 14 load tile[ty][18..19].
        if (gtid.x >= 14)
        {
            int2 p = int2(origin) + int2(gtid.xy) + int2(2, 0);
            tile[gtid.y][gtid.x + 4] = LoadInput(ClampCoord(p, pc.atlasSize), pc.atlasSize, pc.srcData);
        }
    }
    else
    {
        // ── Vertical pass ───────────────────────────────────────────────
        // Interior maps to tile[ty + 2][tx], covering output rows [0, 15].
        int2 base = int2(origin) + int2(gtid.xy);
        tile[gtid.y + 2][gtid.x] = LoadInput(ClampCoord(base, pc.atlasSize), pc.atlasSize, pc.srcData);

        // Top border: threads with ty < 2 load tile[0..1][tx].
        if (gtid.y < 2)
        {
            int2 p = int2(origin) + int2(gtid.xy) - int2(0, 2);
            tile[gtid.y][gtid.x] = LoadInput(ClampCoord(p, pc.atlasSize), pc.atlasSize, pc.srcData);
        }

        // Bottom border: threads with ty >= 14 load tile[18..19][tx].
        if (gtid.y >= 14)
        {
            int2 p = int2(origin) + int2(gtid.xy) + int2(0, 2);
            tile[gtid.y + 4][gtid.x] = LoadInput(ClampCoord(p, pc.atlasSize), pc.atlasSize, pc.srcData);
        }
    }

    // Ensure all LDS writes are visible before any thread reads.
    GroupMemoryBarrierWithGroupSync();

    // ── Phase 2: Compute convolution from LDS ──
    // Only valid threads (within blur region bounds) produce output.
    uint2 localPos = dtid.xy;
    if (localPos.x < pc.atlasSize.x && localPos.y < pc.atlasSize.y)
    {
        float2 result = float2(0.0, 0.0);
        [unroll]
        for (int t = 0; t < 5; ++t)
        {
            if (pc.isHorizontal != 0u)
                result += tile[gtid.y][gtid.x + kOffsets[t] + 2] * kWeights[t];
            else
                result += tile[gtid.y + kOffsets[t] + 2][gtid.x] * kWeights[t];
        }
        StoreOutput(globalPos, pc.atlasSize, result, pc.dstData);
    }
}
