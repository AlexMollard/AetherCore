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
//   offset  0 : DevicePtr<FrameConstantsData> frame
//   offset  8 : DevicePtr<DrawInstanceData>   instanceData
//   offset 16 : DevicePtr<CullDrawInput>      inputCmd   (prototype commands)
//   offset 24 : DevicePtr<uint>               outputCmd  (VkDrawIndexedIndirectCommand array)
//   offset 32 : DevicePtr<CullBatch>          batchDesc
//   offset 40 : uint64                        batchCountAddr (unused, reserved)
//   offset 48 : uint                          totalDrawCount
//   offset 52 : uint                          debugFlags bit0=force all draws visible
//   offset 56 : uint                          _pad0
//   offset 60 : uint                          _pad1

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

    CullDrawInput    cmd  = pc.inputCmd.Load(drawIdx);
    DrawInstanceData inst = pc.instanceData.Load(cmd.firstInstance);
    CullBatch        batch = pc.batchDesc.Load(cmd.batchIndex);

    const bool forceVisible = (pc.debugFlags & kCullDebugForceVisibleBit) != 0u;

    FrameConstantsData* frame = pc.frame.Ptr();
    const bool visible = forceVisible || TestSphereFrustum(inst.worldBoundingSphere, frame);
    const uint localOutputSlot = drawIdx - batch.inputStart;
    if (localOutputSlot >= batch.drawCount)
        return; // guard against malformed batch metadata

    // Write VkDrawIndexedIndirectCommand as 5 consecutive uint32 values.
    // Layout: indexCount | instanceCount | firstIndex | vertexOffset | firstInstance.
    // Culled draws keep their slot but set instanceCount=0 so DrawIndexedIndirect skips them.
    uint* out  = pc.outputCmd.Ptr();
    uint  base = (batch.outputStart + localOutputSlot) * 5u;
    out[base + 0u] = cmd.indexCount;
    out[base + 1u] = visible ? cmd.instanceCount : 0u;
    out[base + 2u] = cmd.firstIndex;
    out[base + 3u] = asuint(cmd.vertexOffset); // int -> uint bit-cast, matches VkDrawIndexedIndirectCommand
    out[base + 4u] = cmd.firstInstance;
}
