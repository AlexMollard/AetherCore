// GPU-driven indirect draw culling pass.
//
// Reads per-draw CullDrawInput prototype commands and DrawInstanceData (with world-space
// bounding spheres) from CPU-written BDA-addressed SSBOs. Frustum-culls each draw against
// the current camera's view-projection matrix from FrameConstantsData. Each draw is written
// to a fixed output slot; culled draws set instanceCount=0 so DrawIndexedIndirect skips them.
//
// Dispatch: ceil(totalDrawCount / 64) groups × 64 threads.
//   - Thread globalId == draw index in [0, totalDrawCount).
//
// Push constants (CullPushConstants, 64 bytes):
//   offset  0 : uint64  frameAddr        BDA -> FrameConstantsData
//   offset  8 : uint64  instanceDataAddr BDA -> DrawInstanceData[]
//   offset 16 : uint64  inputCmdAddr     BDA -> CullDrawInput[] (prototype commands)
//   offset 24 : uint64  outputCmdAddr    BDA -> uint32[] (VkDrawIndexedIndirectCommand array)
//   offset 32 : uint64  batchDescAddr    BDA -> CullBatch[]
//   offset 40 : uint64  batchCountAddr   BDA -> (unused, reserved)
//   offset 48 : uint32  totalDrawCount
//   offset 52 : uint32  debugFlags bit0=force all draws visible
//   offset 56 : uint32  _pad0
//   offset 60 : uint32  _pad1

#include "include/FrameConstants.slangh"
#include "include/RenderContracts.slangh"
#include "include/FrustumCull.slangh"

[[vk::push_constant]] CullPushConstants pc;

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
    FrameConstantsData* fc      = (FrameConstantsData*)(pc.frameAddr);

    CullDrawInput    cmd  = inputs[drawIdx];
    DrawInstanceData inst = instances[cmd.firstInstance];
    CullBatch        batch = batches[cmd.batchIndex];

    const bool forceVisible = (pc.debugFlags & kCullDebugForceVisibleBit) != 0u;

    const bool visible = forceVisible || TestSphereFrustum(inst.worldBoundingSphere, fc->viewProj);
    const uint localOutputSlot = drawIdx - batch.inputStart;
    if (localOutputSlot >= batch.drawCount)
        return; // guard against malformed batch metadata

    // Write VkDrawIndexedIndirectCommand as 5 consecutive uint32 values.
    // Layout: indexCount | instanceCount | firstIndex | vertexOffset | firstInstance.
    // Culled draws keep their slot but set instanceCount=0 so DrawIndexedIndirect skips them.
    uint* out  = (uint*)(pc.outputCmdAddr);
    uint  base = (batch.outputStart + localOutputSlot) * 5u;
    out[base + 0u] = cmd.indexCount;
    out[base + 1u] = visible ? cmd.instanceCount : 0u;
    out[base + 2u] = cmd.firstIndex;
    out[base + 3u] = asuint(cmd.vertexOffset); // int -> uint bit-cast, matches VkDrawIndexedIndirectCommand
    out[base + 4u] = cmd.firstInstance;
}
