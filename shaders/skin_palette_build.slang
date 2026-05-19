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

    // Slang/SPIR-V uses column-major: float4x4(a,b,c,d) sets columns, M[c][r] = column c, row r.
    // Column 0 = image of (1,0,0): (1-2(yy+zz), 2(xy+wz), 2(xz-wy), 0)
    // Column 1 = image of (0,1,0): (2(xy-wz), 1-2(xx+zz), 2(yz+wx), 0)
    // Column 2 = image of (0,0,1): (2(xz+wy), 2(yz-wx), 1-2(xx+yy), 0)
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
    // Column-major: r[i] is column i of the rotation matrix.
    // TRS column j = R * (S * e_j): col 0 = r[0]*sx, col 1 = r[1]*sy, col 2 = r[2]*sz, col 3 = translation.
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

        float4x4 globalM = float4x4(1.0);
        uint safety = 0u;
        int walk = int(nodeIndex);
        while (walk >= 0 && safety < max(1u, job.nodeCount))
        {
            SampledNodePose p = sampled[uint(walk)];
            globalM = mul(ComposeTrs(p.translation, p.rotation, p.scale), globalM);
            walk = nodeParents[uint(walk)];
            ++safety;
        }

        const uint invBindBase = meta.inverseBindOffset / 64u;
        dst[job.dstPaletteOffset + j] = mul(globalM, skinInverseBinds[invBindBase + j]);
    }
}
