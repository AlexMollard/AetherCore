// cull_draws_multi.slang - GPU-driven indirect draw culling pass (multi-frustum).
//
// Like cull_draws.slang but tests each draw against 3 independent cascade
// frustums and writes 3 separate VkDrawIndexedIndirectCommand outputs (one per
// cascade).  Replaces 3× single-frustum dispatches on 3× duplicated data with
// 1 dispatch on shared data.
//
// See cull_draws.slang for the base data layout.  This shader uses
// CullMultiPushConstants instead of CullPushConstants:
//
//   offset  0 : uint64  frameAddrs[3]      BDA -> FrameConstantsData per cascade
//   offset 24 : uint64  instanceDataAddr    BDA -> DrawInstanceData[]
//   offset 32 : uint64  inputCmdAddr        BDA -> CullDrawInput[]
//   offset 40 : uint64  outputCmdAddr       BDA -> output buffer base
//   offset 48 : uint64  batchDescAddr       BDA -> CullBatch[]
//   offset 56 : uint    totalDrawCount
//   offset 60 : uint    outputCascadeStride  bytes between cascade output regions
//   offset 64 : uint    debugFlags
//   offset 68 : uint    _pad0

#include "include/FrameConstants.slangh"
#include "include/RenderContracts.slangh"

[[vk::push_constant]] CullMultiPushConstants pc;

// ── Frustum culling ──────────────────────────────────────────────────────────

// Same as cull_draws.slang.  Returns true if the sphere is inside (or straddling)
// the frustum defined by viewProj.
bool TestSphereFrustum(float4 sphere, float4x4 viewProj)
{
    if (sphere.w <= 0.0)
        return true;

    float4 row0 = float4(viewProj[0][0], viewProj[1][0], viewProj[2][0], viewProj[3][0]);
    float4 row1 = float4(viewProj[0][1], viewProj[1][1], viewProj[2][1], viewProj[3][1]);
    float4 row2 = float4(viewProj[0][2], viewProj[1][2], viewProj[2][2], viewProj[3][2]);
    float4 row3 = float4(viewProj[0][3], viewProj[1][3], viewProj[2][3], viewProj[3][3]);

    float4 planes[6];
    planes[0] = row3 + row0;
    planes[1] = row3 - row0;
    planes[2] = row3 + row1;
    planes[3] = row3 - row1;
    planes[4] = row2;
    planes[5] = row3 - row2;

    float3 center = sphere.xyz;
    float  radius = sphere.w;

    for (int i = 0; i < 6; ++i)
    {
        float3 n    = planes[i].xyz;
        float  d    = planes[i].w;
        float  nLen = length(n);
        if (dot(n, center) + d < -radius * nLen)
            return false;
    }
    return true;
}

// ── Compute entry point ──────────────────────────────────────────────────────

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint drawIdx = tid.x;
    if (drawIdx >= pc.totalDrawCount)
        return;

    CullDrawInput*    inputs    = (CullDrawInput*)(pc.inputCmdAddr);
    DrawInstanceData* instances = (DrawInstanceData*)(pc.instanceDataAddr);
    CullBatch*        batches   = (CullBatch*)(pc.batchDescAddr);
    FrameConstantsData* fc;
    uint8_t* outBase = (uint8_t*)(pc.outputCmdAddr);

    CullDrawInput    cmd  = inputs[drawIdx];
    DrawInstanceData inst = instances[cmd.firstInstance];
    CullBatch        batch = batches[cmd.batchIndex];

    const bool forceVisible = (pc.debugFlags & kCullDebugForceVisibleBit) != 0u;
    const uint localOutputSlot = drawIdx - batch.inputStart;
    if (localOutputSlot >= batch.drawCount)
        return;

    // Test against each cascade frustum and write output independently.
    [unroll]
    for (uint cascade = 0; cascade < kCullMultiFrustumCount; ++cascade)
    {
        fc = (FrameConstantsData*)(pc.frameAddrs[cascade]);
        const bool visible = forceVisible || TestSphereFrustum(inst.worldBoundingSphere, fc->viewProj);

        // Output slot = batch.outputStart + localOutputSlot, offset by cascade * stride.
        uint* out = (uint*)(outBase + cascade * pc.outputCascadeStride);
        uint  base = (batch.outputStart + localOutputSlot) * 5u;
        out[base + 0u] = cmd.indexCount;
        out[base + 1u] = visible ? cmd.instanceCount : 0u;
        out[base + 2u] = cmd.firstIndex;
        out[base + 3u] = asuint(cmd.vertexOffset);
        out[base + 4u] = cmd.firstInstance;
    }
}
