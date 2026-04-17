// skin_palette_build.slang
// Builds a per-frame global skin palette pool.
// Supports two sources per job:
//   1) GPU-sampled node poses (preferred)
//   2) CPU-generated palette matrices (fallback)

#include "include/AnimationContracts.slangh"

[[vk::push_constant]] SkinPalettePush pc;

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
        float4(1.0 - 2.0 * (yy + zz), 2.0 * (xy + wz),       2.0 * (xz - wy),       0.0),
        float4(2.0 * (xy - wz),       1.0 - 2.0 * (xx + zz), 2.0 * (yz + wx),       0.0),
        float4(2.0 * (xz + wy),       2.0 * (yz - wx),       1.0 - 2.0 * (xx + yy), 0.0),
        float4(0.0,                    0.0,                    0.0,                    1.0)
    );
}

float4x4 ComposeTrs(float4 t, float4 q, float4 s)
{
    float4x4 m = QuatToMat(normalize(q));
    m[0] *= s.x;
    m[1] *= s.y;
    m[2] *= s.z;
    m[3] = float4(t.xyz, 1.0);
    return m;
}

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

    float4x4* src = (float4x4*)(job.srcPaletteAddr);
    SampledNodePose* sampled = (SampledNodePose*)(job.sampledPosesAddr);
    float4x4* dst = (float4x4*)(pc.dstPaletteAddr);
    float4x4* prev = (float4x4*)(pc.prevPaletteAddr);
    int* nodeParents = (int*)(pc.nodeParentsAddr);
    GpuSkinMeta* skinMetas = (GpuSkinMeta*)(pc.skinMetasAddr);
    uint* skinJoints = (uint*)(pc.skinJointsAddr);
    float4x4* skinInverseBinds = (float4x4*)(pc.skinInverseBindsAddr);

    const float a = clamp(job.blendAlpha, 0.0, 1.0);
    for (uint j = 0u; j < job.jointCount; ++j)
    {
        const uint dstIndex = job.dstPaletteOffset + j;

        float4x4 current = float4x4(1.0);
        if (job.useSampledPoses != 0u && job.sampledPosesAddr != 0uL && pc.skinMetasAddr != 0uL && pc.skinJointsAddr != 0uL && pc.skinInverseBindsAddr != 0uL)
        {
            GpuSkinMeta meta = skinMetas[job.skinIndex];
            uint skinJointIndex = meta.jointOffset + j;
            uint nodeIndex = skinJoints[skinJointIndex];

            float4x4 globalM = float4x4(1.0);
            uint safety = 0u;
            int walk = int(nodeIndex);
            while (walk >= 0 && safety < max(1u, job.nodeCount))
            {
                SampledNodePose p = sampled[uint(walk)];
                float4x4 localM = ComposeTrs(p.translation, p.rotation, p.scale);
                globalM = mul(localM, globalM);
                walk = (pc.nodeParentsAddr != 0uL) ? nodeParents[uint(walk)] : -1;
                ++safety;
            }

            const uint invBindBase = meta.inverseBindOffset / 64u;
            float4x4 invBind = skinInverseBinds[invBindBase + j];
            current = mul(globalM, invBind);
        }
        else if (job.srcPaletteAddr != 0uL)
        {
            current = src[j];
        }

        if (a >= 0.999f)
        {
            dst[dstIndex] = current;
        }
        else
        {
            float4x4 prevM = prev[dstIndex];
            dst[dstIndex] =
                (1.0f - a) * prevM +
                a * current;
        }
    }
}
