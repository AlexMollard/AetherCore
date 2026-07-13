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

    SkinCopyJob job = pc.jobs.Load(jobIndex);

    if (job.jointCount == 0u)
        return;

    float4x4* dst = pc.dstPalette.Ptr();
    float4x4* globalTransforms = pc.globalTransforms.Ptr();
    GpuSkinMeta* skinMetas = pc.skinMetas.Ptr();
    uint* skinJoints = (uint*)(pc.skinJoints.addr);
    float4x4* skinInverseBinds = pc.skinInverseBinds.Ptr();

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
