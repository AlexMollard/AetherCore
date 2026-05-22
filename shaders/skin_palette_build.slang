// skin_palette_build.slang
// Builds a per-frame global skin palette pool from GPU-sampled node poses.

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

    // Slang targeting Vulkan/SPIR-V: float4x4(a,b,c,d) sets COLUMNS.
    // Column-major, column-vector convention (v' = M * v).
    // mul(M, v) = OpMatrixTimesVector. Translation in col 3.
    return float4x4(
        float4(1.0 - 2.0 * (yy + zz), 2.0 * (xy + wz),       2.0 * (xz - wy),       0.0),
        float4(2.0 * (xy - wz),       1.0 - 2.0 * (xx + zz), 2.0 * (yz + wx),       0.0),
        float4(2.0 * (xz + wy),       2.0 * (yz - wx),       1.0 - 2.0 * (xx + yy), 0.0),
        float4(0.0,                    0.0,                    0.0,                    1.0)
    );
}

float4x4 ComposeTrs(float4 t, float4 q, float4 s)
{
    float4x4 r = QuatToMat(normalize(q));
    // r[i] = column i. Build TRS: cols 0-2 = scaled rotation, col 3 = translation.
    return float4x4(
        float4(r[0][0] * s.x, r[0][1] * s.x, r[0][2] * s.x, 0.0),
        float4(r[1][0] * s.y, r[1][1] * s.y, r[1][2] * s.y, 0.0),
        float4(r[2][0] * s.z, r[2][1] * s.z, r[2][2] * s.z, 0.0),
        float4(t.x,           t.y,            t.z,            1.0)
    );
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

    if (job.jointCount == 0u || job.sampledPosesAddr == 0uL)
        return;

    SampledNodePose* sampled = (SampledNodePose*)(job.sampledPosesAddr);
    float4x4* dst = (float4x4*)(pc.dstPaletteAddr);
    int* nodeParents = (int*)(pc.nodeParentsAddr);
    GpuSkinMeta* skinMetas = (GpuSkinMeta*)(pc.skinMetasAddr);
    uint* skinJoints = (uint*)(pc.skinJointsAddr);
    float4x4* skinInverseBinds = (float4x4*)(pc.skinInverseBindsAddr);

    GpuSkinMeta meta = skinMetas[job.skinIndex];
    for (uint j = 0u; j < job.jointCount; ++j)
    {
        uint nodeIndex = skinJoints[meta.jointOffset + j];

        float4x4 globalM = float4x4(
            float4(1.0, 0.0, 0.0, 0.0),
            float4(0.0, 1.0, 0.0, 0.0),
            float4(0.0, 0.0, 1.0, 0.0),
            float4(0.0, 0.0, 0.0, 1.0));
        uint safety = 0u;
        int walk = int(nodeIndex);
        while (walk >= 0 && safety < max(1u, job.nodeCount))
        {
            SampledNodePose p = sampled[uint(walk)];
            float4x4 localM = ComposeTrs(p.translation, p.rotation, p.scale);
            globalM = mul(localM, globalM);

            // Debug: dump globalM after each step for joint 2 (node 4)
            if (jobIndex == 0u && j == 2u && pc.debugSkinMatricesAddr != 0uL)
            {
                float* debugOut = (float*)(pc.debugSkinMatricesAddr);
                uint outBase = safety * 16u;
                debugOut[outBase + 0]  = globalM[0][0];
                debugOut[outBase + 1]  = globalM[0][1];
                debugOut[outBase + 2]  = globalM[0][2];
                debugOut[outBase + 3]  = globalM[0][3];
                debugOut[outBase + 4]  = globalM[1][0];
                debugOut[outBase + 5]  = globalM[1][1];
                debugOut[outBase + 6]  = globalM[1][2];
                debugOut[outBase + 7]  = globalM[1][3];
                debugOut[outBase + 8]  = globalM[2][0];
                debugOut[outBase + 9]  = globalM[2][1];
                debugOut[outBase + 10] = globalM[2][2];
                debugOut[outBase + 11] = globalM[2][3];
                debugOut[outBase + 12] = globalM[3][0];
                debugOut[outBase + 13] = globalM[3][1];
                debugOut[outBase + 14] = globalM[3][2];
                debugOut[outBase + 15] = globalM[3][3];
            }

            walk = nodeParents[uint(walk)];
            ++safety;
        }

        const uint invBindBase = meta.inverseBindOffset / 64u;
        float4x4 skinMatrix = mul(globalM, skinInverseBinds[invBindBase + j]);
        dst[job.dstPaletteOffset + j] = skinMatrix;

        // Debug: dump first 4 joints of job 0 to debug buffer (skin matrices at offset 0)
        if (jobIndex == 0u && j < 4u && pc.debugSkinMatricesAddr != 0uL)
        {
            float* debugOut = (float*)(pc.debugSkinMatricesAddr);
            uint outBase = j * 16u;
            debugOut[outBase + 0]  = skinMatrix[0][0];
            debugOut[outBase + 1]  = skinMatrix[0][1];
            debugOut[outBase + 2]  = skinMatrix[0][2];
            debugOut[outBase + 3]  = skinMatrix[0][3];
            debugOut[outBase + 4]  = skinMatrix[1][0];
            debugOut[outBase + 5]  = skinMatrix[1][1];
            debugOut[outBase + 6]  = skinMatrix[1][2];
            debugOut[outBase + 7]  = skinMatrix[1][3];
            debugOut[outBase + 8]  = skinMatrix[2][0];
            debugOut[outBase + 9]  = skinMatrix[2][1];
            debugOut[outBase + 10] = skinMatrix[2][2];
            debugOut[outBase + 11] = skinMatrix[2][3];
            debugOut[outBase + 12] = skinMatrix[3][0];
            debugOut[outBase + 13] = skinMatrix[3][1];
            debugOut[outBase + 14] = skinMatrix[3][2];
            debugOut[outBase + 15] = skinMatrix[3][3];
        }

        // Debug: dump intermediate globalM for joint 2 (node 4) at offset 64
        if (jobIndex == 0u && j == 2u && pc.debugSkinMatricesAddr != 0uL)
        {
            float* debugOut = (float*)(pc.debugSkinMatricesAddr);
            uint outBase = 64u + safety * 16u;
            debugOut[outBase + 0]  = globalM[0][0];
            debugOut[outBase + 1]  = globalM[0][1];
            debugOut[outBase + 2]  = globalM[0][2];
            debugOut[outBase + 3]  = globalM[0][3];
            debugOut[outBase + 4]  = globalM[1][0];
            debugOut[outBase + 5]  = globalM[1][1];
            debugOut[outBase + 6]  = globalM[1][2];
            debugOut[outBase + 7]  = globalM[1][3];
            debugOut[outBase + 8]  = globalM[2][0];
            debugOut[outBase + 9]  = globalM[2][1];
            debugOut[outBase + 10] = globalM[2][2];
            debugOut[outBase + 11] = globalM[2][3];
            debugOut[outBase + 12] = globalM[3][0];
            debugOut[outBase + 13] = globalM[3][1];
            debugOut[outBase + 14] = globalM[3][2];
            debugOut[outBase + 15] = globalM[3][3];
        }
    }
}
