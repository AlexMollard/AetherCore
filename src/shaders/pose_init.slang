// Parallel bind-pose initialization for all animator jobs.
// Uses 2D dispatch: (jobIdx, nodeIdx) linearized into (x, y).
// Each thread writes a single node's bind-pose TRS to the sampled poses buffer.
// This eliminates the serial per-job node loop, replacing it with
// a fully parallel write (8x8 threads per workgroup).
//
// Must complete before Animation.SampleClips overwrites channels.

#include "include/AnimationContracts.slangh"

[[vk::push_constant]] PoseInitPush pc;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint jobIdx = dtid.x;
    uint nodeIdx = dtid.y;

    if (jobIdx >= pc.jobCount)
        return;

    AnimatorSampleJob job = pc.animatorJobs.Load(jobIdx);

    if (nodeIdx >= job.nodeCount)
        return;

    float4* bindT = pc.bindTranslations.Ptr();
    float4* bindR = pc.bindRotations.Ptr();
    float4* bindS = pc.bindScales.Ptr();
    SampledNodePose* poses = pc.sampledPoses.Ptr();

    const uint poseIdx = job.nodePoseOffset + nodeIdx;
    poses[poseIdx].translation = bindT[nodeIdx];
    poses[poseIdx].rotation = SafeNormalize(bindR[nodeIdx]);
    poses[poseIdx].scale = bindS[nodeIdx];
}
