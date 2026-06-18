// GPU two-bone IK solver pass.
//
// Each dispatch thread handles one entity's two-bone leg IK (two legs).
// Reads:
//   - Global transforms after animation + blend (from nodeFlatten output)
//   - CPU-written IK ground results (foot target Y positions)
// Produces:
//   - Corrected global transforms for the leg chain written back to the
//     same globalTransforms buffer, feeding into BuildSkinPalette for rendering.
//
// Algorithm:
//   1. Read root/knee/foot world positions from global transforms.
//   2. Read CPU-computed foot target from ikGroundResults buffer.
//   3. Apply two-bone IK: position foot at ground, compute knee on circle.
//   4. Propagate corrections back up through ancestor nodes (hips + ancestors).
//      We recompute local TRS for the modified chain and re-accumulate globals.

#include "include/AnimationContracts.slangh"

[[vk::push_constant]] IkSolvePush pc;

// Transform decomposition utilities.
float4x4 QuatToMat(float4 q)
{
    float x = q.x, y = q.y, z = q.z, w = q.w;
    float xx = x*x, yy = y*y, zz = z*z;
    float xy = x*y, xz = x*z, yz = y*z;
    float wx = w*x, wy = w*y, wz = w*z;
    return float4x4(
        float4(1.0 - 2.0*(yy+zz), 2.0*(xy - wz),  2.0*(xz + wy),  0.0),
        float4(2.0*(xy + wz),  1.0 - 2.0*(xx+zz), 2.0*(yz - wx),  0.0),
        float4(2.0*(xz - wy),  2.0*(yz + wx),  1.0 - 2.0*(xx+yy), 0.0),
        float4(0.0,             0.0,             0.0,             1.0)
    );
}

float4 MatToQuat(float4x4 m)
{
    float tr = m[0][0] + m[1][1] + m[2][2];
    float4 q;
    if (tr > 0.0)
    {
        float s = sqrt(tr + 1.0) * 2.0;
        q = float4(0.25 * s, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s);
    }
    else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
    {
        float s = sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2.0;
        q = float4((m[1][2] - m[2][1]) / s, 0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s);
    }
    else if (m[1][1] > m[2][2])
    {
        float s = sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2.0;
        q = float4((m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s);
    }
    else
    {
        float s = sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2.0;
        q = float4((m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s);
    }
    return normalize(q);
}

float3 ExtractTranslation(float4x4 m)
{
    return float3(m[0][3], m[1][3], m[2][3]);
}

float4x4 ComposeTrs(float3 t, float4 q, float3 s)
{
    float4 qn = normalize(q);
    float4x4 r = QuatToMat(qn);
    return float4x4(
        float4(r[0][0]*s.x, r[0][1]*s.y, r[0][2]*s.z, t.x),
        float4(r[1][0]*s.x, r[1][1]*s.y, r[1][2]*s.z, t.y),
        float4(r[2][0]*s.x, r[2][1]*s.y, r[2][2]*s.z, t.z),
        float4(0.0, 0.0, 0.0, 1.0)
    );
}

float3 TransformPoint(float4x4 m, float3 p)
{
    float4 r = float4(p.x, p.y, p.z, 1.0);
    return float3(
        dot(float4(m[0][0], m[0][1], m[0][2], m[0][3]), r),
        dot(float4(m[1][0], m[1][1], m[1][2], m[1][3]), r),
        dot(float4(m[2][0], m[2][1], m[2][2], m[2][3]), r)
    );
}

float3 TransformVector(float4x4 m, float3 v)
{
    return float3(
        dot(float3(m[0][0], m[0][1], m[0][2]), v),
        dot(float3(m[1][0], m[1][1], m[1][2]), v),
        dot(float3(m[2][0], m[2][1], m[2][2]), v)
    );
}

float3 QuatRotate(float4 q, float3 v)
{
    float3 u = float3(q.x, q.y, q.z);
    float s = q.w;
    return 2.0 * dot(u, v) * u
         + (s*s - dot(u, u)) * v
         + 2.0 * s * cross(u, v);
}

float3 GetWorldTranslation(uint nodeIdx, float4x4* globalTransforms, int* nodeParents, uint poseBase)
{
    return ExtractTranslation(globalTransforms[poseBase + nodeIdx]);
}

float4 GetWorldRotation(uint nodeIdx, float4x4* globalTransforms, int* nodeParents, uint poseBase)
{
    float4x4 m = globalTransforms[poseBase + nodeIdx];
    return MatToQuat(m);
}

float3 GetWorldScale(uint nodeIdx, float4x4* globalTransforms, int* nodeParents, uint poseBase)
{
    float4x4 m = globalTransforms[poseBase + nodeIdx];
    float3 col0 = float3(m[0][0], m[1][0], m[2][0]);
    float3 col1 = float3(m[0][1], m[1][1], m[2][1]);
    float3 col2 = float3(m[0][2], m[1][2], m[2][2]);
    return float3(length(col0), length(col1), length(col2));
}

// Two-bone IK: given root (hip), target (foot ground), and limb lengths,
// compute the mid (knee) position on the circle arc.
float3 SolveTwoBoneIK(float3 root, float3 target, float upperLen, float lowerLen, float bendSign)
{
    float3 dir = target - root;
    float dist = length(dir);
    if (dist >= upperLen + lowerLen - 0.001f)
    {
        float3 nd = (dist > 1e-6f) ? dir / dist : float3(0.0, -1.0, 0.0);
        return root + nd * upperLen;
    }
    if (dist < abs(upperLen - lowerLen) + 0.001f)
    {
        float3 nd = (dist > 1e-6f) ? dir / dist : float3(0.0, -1.0, 0.0);
        return root + nd * upperLen;
    }

    float cosAngle = (upperLen*upperLen + dist*dist - lowerLen*lowerLen) / (2.0 * upperLen * dist);
    cosAngle = clamp(cosAngle, -1.0, 1.0);
    float angle = acos(cosAngle);

    float3 forward = float3(0.0, -1.0, 0.0);
    float3 up = float3(0.0, 0.0, 1.0);
    float3 right = cross(forward, up);
    if (length(right) < 1e-6f)
        right = float3(1.0, 0.0, 0.0);
    right = normalize(right);
    up = cross(right, forward);
    up = normalize(up);

    float3 perp = normalize(dir);
    float3 side = cross(perp, up) * bendSign;
    float3 offset = perp * cos(angle) + side * sin(angle);
    offset = normalize(offset) * upperLen;

    return root + offset;
}

// Propagate global transform changes down the ancestor chain.
// For each ancestor of the modified chain, we:
//   1. Reconstruct the local TRS from the old and new global of the child.
//   2. Re-accumulate from this ancestor down to the descendants.
// We use a simplified approach: just recompute local for this node using
// the parent global (already correct) and write the new local into the buffer,
// then re-flatten the entire sub-hierarchy starting from this node.
// Instead of full re-flatten, we mark the subtree to be recomputed.
//
// For simplicity: find all ancestors from root to modified node, recompute their
// global transforms in a bottom-up fashion since children control parent globals.
void PropagateChainCorrection(
    uint nodeIdx,
    int* nodeParents,
    uint poseBase,
    uint nodeCount,
    float4x4* globalTransforms,
    uint maxIterations)
{
    for (uint iter = 0; iter < maxIterations && nodeIdx != UINT32_MAX; ++iter)
    {
        int parentIdx = nodeParents[nodeIdx];
        if (parentIdx < 0)
            break;

        uint globalNodeIdx = poseBase + nodeIdx;
        uint globalParentIdx = poseBase + uint(parentIdx);

        float4x4 parentM = globalTransforms[globalParentIdx];
        float4x4 nodeM = globalTransforms[globalNodeIdx];

        float3 t = ExtractTranslation(nodeM);
        float4 q = MatToQuat(nodeM);
        float3 s = float3(
            length(float3(nodeM[0][0], nodeM[1][0], nodeM[2][0])),
            length(float3(nodeM[0][1], nodeM[1][1], nodeM[2][1])),
            length(float3(nodeM[0][2], nodeM[1][2], nodeM[2][2]))
        );

        float4x4 invParent = float4x4(
            float4(1.0/parentM[0][0], 0.0, 0.0, 0.0),
            float4(0.0, 1.0/parentM[1][1], 0.0, 0.0),
            float4(0.0, 0.0, 1.0/parentM[2][2], 0.0),
            float4(-parentM[0][3], -parentM[1][3], -parentM[2][3], 1.0)
        );
        float4x4 localM = mul(invParent, nodeM);

        float3 lt = ExtractTranslation(localM);
        float4 lq = MatToQuat(localM);
        float3 ls = float3(
            length(float3(localM[0][0], localM[1][0], localM[2][0])),
            length(float3(localM[0][1], localM[1][1], localM[2][1])),
            length(float3(localM[0][2], localM[1][2], localM[2][2]))
        );

        globalTransforms[globalNodeIdx] = ComposeTrs(lt, lq, ls);
        nodeIdx = uint(parentIdx);
    }
}

// Full re-flatten of the skeleton hierarchy from root down.
// Writes to globalTransforms in depth-sorted node order.
void ReflattenHierarchy(
    uint nodeIdx,
    int* nodeParents,
    uint poseBase,
    uint nodeCount,
    float4x4* globalTransforms,
    uint* depthSortedNodes,
    uint depthSortedCount)
{
    if (nodeIdx >= nodeCount)
        return;

    float4x4 parentM = float4x4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
    uint pidx = uint(nodeParents[nodeIdx]);
    if (pidx != UINT32_MAX && pidx < nodeCount)
        parentM = globalTransforms[poseBase + pidx];

    float4x4 localM = globalTransforms[poseBase + nodeIdx];
    globalTransforms[poseBase + nodeIdx] = mul(parentM, localM);

    for (uint i = 0; i < depthSortedCount; ++i)
    {
        uint n = depthSortedNodes[i];
        if (n >= nodeCount)
            continue;
        if (uint(nodeParents[n]) == nodeIdx)
            ReflattenHierarchy(n, nodeParents, poseBase, nodeCount, globalTransforms, depthSortedNodes, depthSortedCount);
    }
}

// ─────────────────────────────────────────────────────────────────────────────

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint jobIdx = dtid.x;
    if (jobIdx >= pc.jobCount)
        return;

    IkSolveJob* jobs = pc.ikJobs.Ptr();
    IkGroundResult* results = (IkGroundResult*)(pc.ikGroundResults.addr);
    int* nodeParents = pc.nodeParents.Ptr();
    uint* depthSortedNodes = pc.depthSortedNodes.Ptr();
    float4x4* globalTransforms = pc.globalTransforms.Ptr();

    IkSolveJob job = jobs[jobIdx];

    uint poseBase = 0;

    uint leftFootResultIdx = jobIdx * 2 + 0;
    uint rightFootResultIdx = jobIdx * 2 + 1;

    IkGroundResult leftResult = results[leftFootResultIdx];
    IkGroundResult rightResult = results[rightFootResultIdx];

    if (job.leftFootNodeIdx == UINT32_MAX && job.rightFootNodeIdx == UINT32_MAX)
        return;

    float3 origHipsPos = ExtractTranslation(globalTransforms[poseBase + job.hipsNodeIdx]);
    float4 origHipsRot = MatToQuat(globalTransforms[poseBase + job.hipsNodeIdx]);

    float3 correction = float3(0.0, 0.0, 0.0);

    // ── Left leg IK ───────────────────────────────────────────────────────────
    if (job.leftFootNodeIdx != UINT32_MAX && leftResult.footOffsetY != 0.0f)
    {
        float3 rootPos = ExtractTranslation(globalTransforms[poseBase + job.hipsNodeIdx]);
        float3 footPos = ExtractTranslation(globalTransforms[poseBase + job.leftFootNodeIdx]);
        float3 kneePos = ExtractTranslation(globalTransforms[poseBase + job.leftKneeNodeIdx]);

        float3 targetFootPos = footPos;
        targetFootPos.y = leftResult.groundY;

        float3 newKneePos = SolveTwoBoneIK(rootPos, targetFootPos, job.leftUpperLegLen, job.leftLowerLegLen, job.leftKneeBendSign);

        float3 footOffset = targetFootPos - footPos;
        correction = footOffset;

        float3 newFootPos = targetFootPos;

        globalTransforms[poseBase + job.leftKneeNodeIdx] = float4x4(
            float4(1,0,0,newKneePos.x), float4(0,1,0,newKneePos.y), float4(0,0,1,newKneePos.z), float4(0,0,0,1)
        );
        globalTransforms[poseBase + job.leftFootNodeIdx] = float4x4(
            float4(1,0,0,newFootPos.x), float4(0,1,0,newFootPos.y), float4(0,0,1,newFootPos.z), float4(0,0,0,1)
        );
    }

    // ── Right leg IK ──────────────────────────────────────────────────────────
    if (job.rightFootNodeIdx != UINT32_MAX && rightResult.footOffsetY != 0.0f)
    {
        float3 rootPos = ExtractTranslation(globalTransforms[poseBase + job.hipsNodeIdx]);
        float3 footPos = ExtractTranslation(globalTransforms[poseBase + job.rightFootNodeIdx]);
        float3 kneePos = ExtractTranslation(globalTransforms[poseBase + job.rightKneeNodeIdx]);

        float3 targetFootPos = footPos;
        targetFootPos.y = rightResult.groundY;

        float3 newKneePos = SolveTwoBoneIK(rootPos, targetFootPos, job.rightUpperLegLen, job.rightLowerLegLen, job.rightKneeBendSign);

        float3 footOffset = targetFootPos - footPos;
        correction = correction * 0.5 + footOffset * 0.5;

        float3 newFootPos = targetFootPos;

        globalTransforms[poseBase + job.rightKneeNodeIdx] = float4x4(
            float4(1,0,0,newKneePos.x), float4(0,1,0,newKneePos.y), float4(0,0,1,newKneePos.z), float4(0,0,0,1)
        );
        globalTransforms[poseBase + job.rightFootNodeIdx] = float4x4(
            float4(1,0,0,newFootPos.x), float4(0,1,0,newFootPos.y), float4(0,0,1,newFootPos.z), float4(0,0,0,1)
        );
    }

    if (job.hipsNodeIdx != UINT32_MAX && (leftResult.footOffsetY != 0.0f || rightResult.footOffsetY != 0.0f))
    {
        float3 hipsPos = ExtractTranslation(globalTransforms[poseBase + job.hipsNodeIdx]);
        float4 hipsQuat = MatToQuat(globalTransforms[poseBase + job.hipsNodeIdx]);
        float3 hipsScale = float3(
            length(float3(globalTransforms[poseBase + job.hipsNodeIdx][0][0], globalTransforms[poseBase + job.hipsNodeIdx][1][0], globalTransforms[poseBase + job.hipsNodeIdx][2][0])),
            length(float3(globalTransforms[poseBase + job.hipsNodeIdx][0][1], globalTransforms[poseBase + job.hipsNodeIdx][1][1], globalTransforms[poseBase + job.hipsNodeIdx][2][1])),
            length(float3(globalTransforms[poseBase + job.hipsNodeIdx][0][2], globalTransforms[poseBase + job.hipsNodeIdx][1][2], globalTransforms[poseBase + job.hipsNodeIdx][2][2]))
        );
        float3 newHipsPos = hipsPos + correction;
        globalTransforms[poseBase + job.hipsNodeIdx] = ComposeTrs(newHipsPos, hipsQuat, hipsScale);
    }
}