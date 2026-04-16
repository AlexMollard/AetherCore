// cull_draws.slang — GPU-driven indirect draw culling pass.
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
//   offset  0 : uint64  frameAddr        BDA → FrameConstantsData
//   offset  8 : uint64  instanceDataAddr BDA → DrawInstanceData[]
//   offset 16 : uint64  inputCmdAddr     BDA → CullDrawInput[] (prototype commands)
//   offset 24 : uint64  outputCmdAddr    BDA → uint32[] (VkDrawIndexedIndirectCommand array)
//   offset 32 : uint64  batchDescAddr    BDA → CullBatch[]
//   offset 40 : uint64  batchCountAddr   BDA → (unused, reserved)
//   offset 48 : uint32  totalDrawCount
//   offset 52 : uint32  debugFlags bit0=force all draws visible
//   offset 56 : uint32  _pad0
//   offset 60 : uint32  _pad1

#include "include/FrameConstants.slangh"

// ── GPU structs (offsets must match their C++ counterparts) ──────────────────

struct DrawInstanceData
{
    [[vk::offset(0)]]  float4x4 model;
    [[vk::offset(64)]] uint     materialIndex;
    [[vk::offset(68)]] uint     _pad0;
    [[vk::offset(72)]] uint64_t skinBufferAddr;
    [[vk::offset(80)]] float4   worldBoundingSphere; // xyz=world center, w=radius; w<=0 → skip cull
};

struct CullDrawInput
{
    [[vk::offset(0)]]  uint indexCount;
    [[vk::offset(4)]]  uint instanceCount;
    [[vk::offset(8)]]  uint firstIndex;
    [[vk::offset(12)]] int  vertexOffset;
    [[vk::offset(16)]] uint firstInstance;  // index into DrawInstanceData[]
    [[vk::offset(20)]] uint batchIndex;     // which output batch this draw belongs to
    [[vk::offset(24)]] uint _pad0;          // must match C++ CullDrawInput (sizeof == 32)
    [[vk::offset(28)]] uint _pad1;
};

struct CullBatch
{
    [[vk::offset(0)]]  uint inputStart;   // first draw index in CullDrawInput[]
    [[vk::offset(4)]]  uint drawCount;    // total draws = max output capacity
    [[vk::offset(8)]]  uint outputStart;  // first slot in output VkDrawIndexedIndirectCommand[]
    [[vk::offset(12)]] uint _pad;
};

struct CullPush
{
    uint64_t frameAddr;
    uint64_t instanceDataAddr;
    uint64_t inputCmdAddr;
    uint64_t outputCmdAddr;
    uint64_t batchDescAddr;
    uint64_t batchCountAddr;
    uint32_t totalDrawCount;
    uint32_t debugFlags;
    uint32_t _pad0;
    uint32_t _pad1;
};
[[vk::push_constant]] CullPush pc;

static const uint kCullDebugForceVisibleBit = 1u;

// ── Frustum culling ──────────────────────────────────────────────────────────

// Test a world-space bounding sphere against the 6 frustum planes derived from viewProj.
// Uses the Gribb-Hartmann method for Vulkan (column-major matrix, depth range [0, 1]).
// Returns true if the sphere is potentially visible (not fully outside any plane).
// sphere.w <= 0 means no valid bounds — always returns true.
bool TestSphereFrustum(float4 sphere, float4x4 viewProj)
{
    if (sphere.w <= 0.0)
        return true;

    // Reconstruct matrix rows from column-major storage.
    // viewProj[col][row] in Slang/SPIR-V, so row i = (vp[0][i], vp[1][i], vp[2][i], vp[3][i]).
    float4 row0 = float4(viewProj[0][0], viewProj[1][0], viewProj[2][0], viewProj[3][0]);
    float4 row1 = float4(viewProj[0][1], viewProj[1][1], viewProj[2][1], viewProj[3][1]);
    float4 row2 = float4(viewProj[0][2], viewProj[1][2], viewProj[2][2], viewProj[3][2]);
    float4 row3 = float4(viewProj[0][3], viewProj[1][3], viewProj[2][3], viewProj[3][3]);

    // Plane equations (unnormalized): plane[i] = float4(normal.xyz, d) where
    // a point P is inside if dot(normal, P) + d >= 0.
    float4 planes[6];
    planes[0] = row3 + row0; // left
    planes[1] = row3 - row0; // right
    planes[2] = row3 + row1; // bottom  (Vulkan +Y down convention)
    planes[3] = row3 - row1; // top
    planes[4] = row2;        // near    (Vulkan depth [0, 1])
    planes[5] = row3 - row2; // far

    float3 center = sphere.xyz;
    float  radius = sphere.w;

    for (int i = 0; i < 6; ++i)
    {
        float3 n    = planes[i].xyz;
        float  d    = planes[i].w;
        float  nLen = length(n);
        // Signed distance from plane = (dot(n, center) + d) / nLen.
        // Fully outside if distance < -radius.
        if (dot(n, center) + d < -radius * nLen)
            return false;
    }
    return true;
}

// ── Compute entry point ──────────────────────────────────────────────────────

[shader("compute")]
[numthreads(64, 1, 1)]
void cullDraws(uint3 tid : SV_DispatchThreadID)
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
    out[base + 3u] = asuint(cmd.vertexOffset); // int → uint bit-cast, matches VkDrawIndexedIndirectCommand
    out[base + 4u] = cmd.firstInstance;
}
