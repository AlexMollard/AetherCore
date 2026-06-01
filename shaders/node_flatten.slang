// Level-by-level hierarchy flattening compute shader.
// Dispatched once per depth level per AnimationDatabase batch.
// Uses 2D linearization: dtid.x encodes (jobIdx, nodeIdx) within batch.
//
// For each node at the current depth level:
//   - Reads SampledNodePose from sampledPosesAddr + poseBase
//   - Composes local TRS matrix
//   - Reads parent's global transform (already written at lower depth)
//   - Accumulates: globalM = mul(localM, parentGlobalM)
//   - Writes per-node float4x4 for O(1) consumption by BuildSkinPalette

#include "include/AnimationContracts.slangh"

[[vk::push_constant]] NodeFlattenPush pc;

float4x4 QuatToMat(float4 q)
{
    float x = q.x;
    float y = q.y;
    float z = q.z;
    float w = q.w;
    float xx = x * x;
    float yy = y * y;
    float zz = z * z;
    float xy = x * y;
    float xz = x * z;
    float yz = y * z;
    float wx = w * x;
    float wy = w * y;
    float wz = w * z;

    return float4x4(
        float4(1.0 - 2.0 * (yy + zz), 2.0 * (xy - wz),       2.0 * (xz + wy),       0.0),
        float4(2.0 * (xy + wz),       1.0 - 2.0 * (xx + zz), 2.0 * (yz - wx),       0.0),
        float4(2.0 * (xz - wy),       2.0 * (yz + wx),       1.0 - 2.0 * (xx + yy), 0.0),
        float4(0.0,                    0.0,                    0.0,                    1.0)
    );
}

// NOTE: In HLSL semantics, r[row][col] accesses row `row`, column `col`.
// Scale is applied per-column: output column j gets s[j] applied to ALL rows.
// This MUST match the original skin_palette_build.slang ComposeTrs.
float4x4 ComposeTrs(float4 t, float4 q, float4 s)
{
    // Guard against zero quaternions (e.g. from cleared buffer slots).
    // normalize(zero) = 0/0 = NaN which propagates to skin matrices.
    float4 qn = (q.x == 0.0f && q.y == 0.0f && q.z == 0.0f && q.w == 0.0f)
        ? float4(0.0f, 0.0f, 0.0f, 1.0f)
        : normalize(q);
    float4x4 r = QuatToMat(qn);
    return float4x4(
        float4(r[0][0] * s.x, r[0][1] * s.y, r[0][2] * s.z, t.x),
        float4(r[1][0] * s.x, r[1][1] * s.y, r[1][2] * s.z, t.y),
        float4(r[2][0] * s.x, r[2][1] * s.y, r[2][2] * s.z, t.z),
        float4(0.0,           0.0,            0.0,            1.0)
    );
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint linearIdx = dtid.x;
    uint jobIdx = linearIdx / pc.nodeCount;
    uint localNodeIdx = linearIdx % pc.nodeCount;
    if (jobIdx >= pc.batchJobCount || localNodeIdx >= pc.nodeCount)
        return;

    AnimatorSampleJob* jobs = (AnimatorSampleJob*)(pc.animatorJobsAddr);
    uint poseBase = jobs[pc.batchStartJob + jobIdx].nodePoseOffset;

    uint* depthSortedNodes = (uint*)(pc.depthSortedNodesAddr);
    uint nodeIdx = depthSortedNodes[pc.depthOffset + localNodeIdx];
    uint globalNodeIdx = poseBase + nodeIdx;

    SampledNodePose* sampled = (SampledNodePose*)(pc.sampledPosesAddr);
    int* nodeParents = (int*)(pc.nodeParentsAddr);
    float4x4* globalTransforms = (float4x4*)(pc.globalTransformsAddr);

    SampledNodePose p = sampled[globalNodeIdx];
    float4x4 localM = ComposeTrs(p.translation, p.rotation, p.scale);

    int parentIdx = nodeParents[nodeIdx];
    if (parentIdx >= 0)
    {
        uint globalParentIdx = poseBase + uint(parentIdx);
        float4x4 parentM = globalTransforms[globalParentIdx];
        // parentM * localM matches hierarchy walk order: root→leaf accumulation
        globalTransforms[globalNodeIdx] = mul(parentM, localM);
    }
    else
    {
        globalTransforms[globalNodeIdx] = localM;
    }
}
