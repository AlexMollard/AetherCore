// GPU animation blend pass.
//
// Reads AnimatorBlendJob entries (one per entity) from the GPU buffer,
// samples the primary clip at primaryTime for all nodes, then if
// secondaryClipIndex is non-zero and blendWeight < 1.0, also samples
// the secondary clip and lerps the two poses together:
//   - Translations: lerp by (1 - blendWeight)
//   - Rotations: slerp then Nlerp
//
// Output is flat TRS node poses written to sampledPosesAddr, which
// feeds directly into nodeFlatten for skeleton evaluation.

#include "include/AnimationContracts.slangh"

[[vk::push_constant]] AnimationBlendPush pc;

// ─────────────────────────────────────────────────────────────────────────────

struct Bracket
{
    uint lo;
    uint hi;
    float alpha;
};

Bracket FindBracket(uint timesOffset, uint count, float t, float* allTimes)
{
    Bracket result;
    result.lo = 0;
    result.hi = 0;
    result.alpha = 0.0f;

    if (count == 0)
        return result;

    float firstTime = allTimes[timesOffset];
    if (t <= firstTime)
        return result;

    float lastTime = allTimes[timesOffset + count - 1];
    if (t >= lastTime)
    {
        result.lo = count - 1;
        result.hi = count - 1;
        result.alpha = 1.0f;
        return result;
    }

    uint lo = 0u;
    uint hi = count - 1u;
    while (lo < hi - 1u)
    {
        uint mid = (lo + hi) >> 1u;
        float midTime = allTimes[timesOffset + mid];
        if (t >= midTime)
            lo = mid;
        else
            hi = mid;
    }

    result.lo = lo;
    result.hi = hi;
    float t0 = allTimes[timesOffset + lo];
    float t1 = allTimes[timesOffset + hi];
    float range = t1 - t0;
    result.alpha = (range > 1e-6f) ? (t - t0) / range : 0.0f;
    return result;
}

float4 Nlerp(float4 q1, float4 q2, float t)
{
    if (dot(q1, q2) < 0.0f)
        q2 = -q2;
    return SafeNormalize(lerp(q1, q2, t));
}

// Sample a single clip at a given time, writing TRS to the output pose array.
// nodePoseOffset must match the job's nodePoseOffset so the flat pose buffer
// stays contiguous for the nodeFlatten pass.
void SampleClip(
    GpuClip clip,
    float sampleTime,
    uint nodeCount,
    uint nodePoseOffset,
    GpuChannel* channels,
    float* times,
    float4* values,
    SampledNodePose* outPoses)
{
    for (uint chIdx = 0u; chIdx < clip.channelCount; ++chIdx)
    {
        uint globalChIdx = clip.channelOffset + chIdx;
        GpuChannel channel = channels[globalChIdx];

        uint nodeIdx = channel.nodeIndex;
        if (nodeIdx >= nodeCount)
            continue;

        uint poseIdx = nodePoseOffset + nodeIdx;

        if (channel.animPath == 0u) // Translation
        {
            Bracket bracket = FindBracket(channel.timesOffset / 4u, channel.timesCount, sampleTime, times);
            uint valIdx0 = channel.valuesOffset / 16u + bracket.lo;
            uint valIdx1 = channel.valuesOffset / 16u + bracket.hi;
            float a = (channel.interpolation == 1u) ? 0.0f : bracket.alpha;
            outPoses[poseIdx].translation = lerp(values[valIdx0], values[valIdx1], a);
        }
        else if (channel.animPath == 1u) // Rotation
        {
            Bracket bracket = FindBracket(channel.timesOffset / 4u, channel.timesCount, sampleTime, times);
            uint valIdx0 = channel.valuesOffset / 16u + bracket.lo;
            uint valIdx1 = channel.valuesOffset / 16u + bracket.hi;
            outPoses[poseIdx].rotation = (channel.interpolation == 1u)
                ? SafeNormalize(values[valIdx0])
                : Nlerp(values[valIdx0], values[valIdx1], bracket.alpha);
        }
        else if (channel.animPath == 2u) // Scale
        {
            Bracket bracket = FindBracket(channel.timesOffset / 4u, channel.timesCount, sampleTime, times);
            uint valIdx0 = channel.valuesOffset / 16u + bracket.lo;
            uint valIdx1 = channel.valuesOffset / 16u + bracket.hi;
            float a = (channel.interpolation == 1u) ? 0.0f : bracket.alpha;
            outPoses[poseIdx].scale = lerp(values[valIdx0], values[valIdx1], a);
        }
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

    AnimatorBlendJob* jobs = (AnimatorBlendJob*)(pc.blendJobsAddr);
    AnimatorBlendJob job = jobs[jobIdx];

    GpuClip* clips = (GpuClip*)(pc.animDbClipsAddr);
    GpuChannel* channels = (GpuChannel*)(pc.animDbChannelsAddr);
    float* times = (float*)(pc.animDbTimesAddr);
    float4* values = (float4*)(pc.animDbValuesAddr);
    SampledNodePose* poses = (SampledNodePose*)(pc.sampledPosesAddr);

    const uint poseBase = job.nodePoseOffset;

    if (job.primaryClipIndex >= job.clipCount)
        return;

    GpuClip primaryClip = clips[job.primaryClipIndex];

    // Sample primary clip into a temp buffer
    SampledNodePose tempPrimary;
    tempPrimary.translation = float4(0, 0, 0, 0);
    tempPrimary.rotation = float4(0, 0, 0, 1);
    tempPrimary.scale = float4(1, 1, 1, 0);

    if (job.secondaryClipIndex != 0u && job.blendWeight < 0.9999f)
    {
        // Both clips active — sample secondary into temp, blend into final
        GpuClip secondaryClip = clips[job.secondaryClipIndex];

        float invBlend = 1.0f - job.blendWeight;

        for (uint chIdx = 0u; chIdx < primaryClip.channelCount; ++chIdx)
        {
            uint globalChIdx = primaryClip.channelOffset + chIdx;
            GpuChannel channel = channels[globalChIdx];

            uint nodeIdx = channel.nodeIndex;
            if (nodeIdx >= job.nodeCount)
                continue;

            uint poseIdx = poseBase + nodeIdx;

            if (channel.animPath == 0u) // Translation — linear lerp
            {
                Bracket bracket = FindBracket(channel.timesOffset / 4u, channel.timesCount, job.primaryTime, times);
                uint valIdx0 = channel.valuesOffset / 16u + bracket.lo;
                uint valIdx1 = channel.valuesOffset / 16u + bracket.hi;
                float a = (channel.interpolation == 1u) ? 0.0f : bracket.alpha;
                float4 primaryTrans = lerp(values[valIdx0], values[valIdx1], a);

                Bracket secBracket = FindBracket(channel.timesOffset / 4u, channel.timesCount, job.secondaryTime, times);
                uint secValIdx0 = channel.valuesOffset / 16u + secBracket.lo;
                uint secValIdx1 = channel.valuesOffset / 16u + secBracket.hi;
                float secA = (channel.interpolation == 1u) ? 0.0f : secBracket.alpha;
                float4 secondaryTrans = lerp(values[secValIdx0], values[secValIdx1], secA);

                poses[poseIdx].translation = lerp(primaryTrans, secondaryTrans, invBlend);
            }
            else if (channel.animPath == 1u) // Rotation — Nlerp blend
            {
                Bracket bracket = FindBracket(channel.timesOffset / 4u, channel.timesCount, job.primaryTime, times);
                uint valIdx0 = channel.valuesOffset / 16u + bracket.lo;
                uint valIdx1 = channel.valuesOffset / 16u + bracket.hi;
                float4 primaryRot = (channel.interpolation == 1u)
                    ? SafeNormalize(values[valIdx0])
                    : Nlerp(values[valIdx0], values[valIdx1], bracket.alpha);

                Bracket secBracket = FindBracket(channel.timesOffset / 4u, channel.timesCount, job.secondaryTime, times);
                uint secValIdx0 = channel.valuesOffset / 16u + secBracket.lo;
                uint secValIdx1 = channel.valuesOffset / 16u + secBracket.hi;
                float4 secondaryRot = (channel.interpolation == 1u)
                    ? SafeNormalize(values[secValIdx0])
                    : Nlerp(values[secValIdx0], values[secValIdx1], secBracket.alpha);

                poses[poseIdx].rotation = Nlerp(primaryRot, secondaryRot, invBlend);
            }
            else if (channel.animPath == 2u) // Scale
            {
                Bracket bracket = FindBracket(channel.timesOffset / 4u, channel.timesCount, job.primaryTime, times);
                uint valIdx0 = channel.valuesOffset / 16u + bracket.lo;
                uint valIdx1 = channel.valuesOffset / 16u + bracket.hi;
                float a = (channel.interpolation == 1u) ? 0.0f : bracket.alpha;
                float4 primaryScale = lerp(values[valIdx0], values[valIdx1], a);

                Bracket secBracket = FindBracket(channel.timesOffset / 4u, channel.timesCount, job.secondaryTime, times);
                uint secValIdx0 = channel.valuesOffset / 16u + secBracket.lo;
                uint secValIdx1 = channel.valuesOffset / 16u + secBracket.hi;
                float secA = (channel.interpolation == 1u) ? 0.0f : secBracket.alpha;
                float4 secondaryScale = lerp(values[secValIdx0], values[secValIdx1], secA);

                poses[poseIdx].scale = lerp(primaryScale, secondaryScale, invBlend);
            }
        }
    }
    else
    {
        // No secondary clip — copy primary clip directly with fast channel loop
        for (uint chIdx = 0u; chIdx < primaryClip.channelCount; ++chIdx)
        {
            uint globalChIdx = primaryClip.channelOffset + chIdx;
            GpuChannel channel = channels[globalChIdx];

            uint nodeIdx = channel.nodeIndex;
            if (nodeIdx >= job.nodeCount)
                continue;

            uint poseIdx = poseBase + nodeIdx;

            if (channel.animPath == 0u)
            {
                Bracket bracket = FindBracket(channel.timesOffset / 4u, channel.timesCount, job.primaryTime, times);
                uint valIdx0 = channel.valuesOffset / 16u + bracket.lo;
                uint valIdx1 = channel.valuesOffset / 16u + bracket.hi;
                float a = (channel.interpolation == 1u) ? 0.0f : bracket.alpha;
                poses[poseIdx].translation = lerp(values[valIdx0], values[valIdx1], a);
            }
            else if (channel.animPath == 1u)
            {
                Bracket bracket = FindBracket(channel.timesOffset / 4u, channel.timesCount, job.primaryTime, times);
                uint valIdx0 = channel.valuesOffset / 16u + bracket.lo;
                uint valIdx1 = channel.valuesOffset / 16u + bracket.hi;
                poses[poseIdx].rotation = (channel.interpolation == 1u)
                    ? SafeNormalize(values[valIdx0])
                    : Nlerp(values[valIdx0], values[valIdx1], bracket.alpha);
            }
            else if (channel.animPath == 2u)
            {
                Bracket bracket = FindBracket(channel.timesOffset / 4u, channel.timesCount, job.primaryTime, times);
                uint valIdx0 = channel.valuesOffset / 16u + bracket.lo;
                uint valIdx1 = channel.valuesOffset / 16u + bracket.hi;
                float a = (channel.interpolation == 1u) ? 0.0f : bracket.alpha;
                poses[poseIdx].scale = lerp(values[valIdx0], values[valIdx1], a);
            }
        }
    }
}