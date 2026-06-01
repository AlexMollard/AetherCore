// Builds a per-frame global skin palette pool from pre-flattened node transforms.
// Reads per-node float4x4 global transforms (written by node_flatten.slang)
// and combines each with the skin's inverse bind matrix.

#include "include/AnimationContracts.slangh"

[[vk::push_constant]] SkinPalettePush pc;

[shader("compute")]
[numthreads(64, 1, 1)]
void buildSkinPalette(uint3 tid : SV_DispatchThreadID)
{
    const uint jobIndex = tid.x;
    if (jobIndex >= pc.jobCount)
        return;

    SkinCopyJob* jobs = (SkinCopyJob*)(pc.jobsAddr);
    SkinCopyJob job = jobs[jobIndex];

    if (job.jointCount == 0u)
        return;

    float4x4* dst = (float4x4*)(pc.dstPaletteAddr);
    float4x4* globalTransforms = (float4x4*)(pc.globalTransformsAddr);
    GpuSkinMeta* skinMetas = (GpuSkinMeta*)(pc.skinMetasAddr);
    uint* skinJoints = (uint*)(pc.skinJointsAddr);
    float4x4* skinInverseBinds = (float4x4*)(pc.skinInverseBindsAddr);

    GpuSkinMeta meta = skinMetas[job.skinIndex];
    const uint poseBase = job.nodePoseOffset;
    for (uint j = 0u; j < job.jointCount; ++j)
    {
        uint nodeIndex = skinJoints[meta.jointOffset + j];
        float4x4 globalM = globalTransforms[poseBase + nodeIndex];

        const uint invBindBase = meta.inverseBindOffset / 64u;
        float4x4 skinMatrix = mul(globalM, skinInverseBinds[invBindBase + j]);
        dst[job.dstPaletteOffset + j] = skinMatrix;
    }
}
