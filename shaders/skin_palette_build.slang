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
    return float4x4(
        float4(r[0][0] * s.x, r[0][1] * s.x, r[0][2] * s.x, 0.0),
        float4(r[1][0] * s.y, r[1][1] * s.y, r[1][2] * s.y, 0.0),
        float4(r[2][0] * s.z, r[2][1] * s.z, r[2][2] * s.z, 0.0),
        float4(t.x,           t.y,            t.z,            1.0)
    );
}

void WriteMatrix(float* out, uint base, float4x4 m)
{
    out[base + 0]  = m[0][0]; out[base + 1]  = m[0][1];
    out[base + 2]  = m[0][2]; out[base + 3]  = m[0][3];
    out[base + 4]  = m[1][0]; out[base + 5]  = m[1][1];
    out[base + 6]  = m[1][2]; out[base + 7]  = m[1][3];
    out[base + 8]  = m[2][0]; out[base + 9]  = m[2][1];
    out[base + 10] = m[2][2]; out[base + 11] = m[2][3];
    out[base + 12] = m[3][0]; out[base + 13] = m[3][1];
    out[base + 14] = m[3][2]; out[base + 15] = m[3][3];
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

    // Debug: for joint 2 (node 4), dump hierarchy walk step by step
    // Layout: offset 0 = ComposeTrs(node4), offset 16 = invBind[2] from GPU,
    // offset 32 = globalM after step 0, offset 48 = globalM after step 1,
    // offset 64 = globalM after step 2, offset 80 = globalM after step 3,
    // offset 96 = final skinMatrix for joint 2
    if (jobIndex == 0u && pc.debugSkinMatricesAddr != 0uL)
    {
        float* debugOut = (float*)(pc.debugSkinMatricesAddr);

        GpuSkinMeta meta = skinMetas[job.skinIndex];
        uint nodeIndex = skinJoints[meta.jointOffset + 2u];

        // Dump invBind[2] as read by shader
        const uint invBindBase = meta.inverseBindOffset / 64u;
        WriteMatrix(debugOut, 16u, skinInverseBinds[invBindBase + 2u]);

        float4x4 globalM = float4x4(
            float4(1.0, 0.0, 0.0, 0.0),
            float4(0.0, 1.0, 0.0, 0.0),
            float4(0.0, 0.0, 1.0, 0.0),
            float4(0.0, 0.0, 0.0, 1.0));

        uint safety = 0u;
        int walk = int(nodeIndex);
        uint stepOffset = 32u;
        while (walk >= 0 && safety < max(1u, job.nodeCount) && safety < 4u)
        {
            SampledNodePose p = sampled[uint(walk)];
            float4x4 localM = ComposeTrs(p.translation, p.rotation, p.scale);

            // Dump ComposeTrs for first step
            if (safety == 0u)
            {
                WriteMatrix(debugOut, 0u, localM);
            }

            globalM = mul(localM, globalM);
            WriteMatrix(debugOut, stepOffset, globalM);
            stepOffset += 16u;

            walk = nodeParents[uint(walk)];
            ++safety;
        }

        // Final skin matrix for joint 2
        float4x4 skinMatrix = mul(globalM, skinInverseBinds[invBindBase + 2u]);
        WriteMatrix(debugOut, 96u, skinMatrix);
    }

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
            walk = nodeParents[uint(walk)];
            ++safety;
        }

        const uint invBindBase = meta.inverseBindOffset / 64u;
        float4x4 skinMatrix = mul(globalM, skinInverseBinds[invBindBase + j]);
        dst[job.dstPaletteOffset + j] = skinMatrix;
    }
}
