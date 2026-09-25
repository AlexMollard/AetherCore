// Animation clip sampling compute shader.
// 
// Samples animation keyframes and computes TRS values for all animators.
// Results are stored per-animator for later use by skeleton evaluation.
//
// Input:
//   - Animation database (clips, channels, times, values)
//   - Animator state (clip index, time, playback speed)
// Output:
//   - Per-animator TRS samples (translation, rotation as quat, scale)

#include "include/AnimationContracts.slangh"

[[vk::push_constant]] AnimationSamplePush pc;

// ─────────────────────────────────────────────────────────────────────────────

// Find bracketing keyframes for time t in sorted times array.
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

// Nlerp two quaternions (in xyzw format as float4).
// Faster approximation of Slerp - visually equivalent for typical animation
// keyframe density, avoids acos/sin/divide.
float4 Nlerp(float4 q1, float4 q2, float t)
{
    if (dot(q1, q2) < 0.0f)
        q2 = -q2;
    return SafeNormalize(lerp(q1, q2, t));
}

float4 SampleChannelValue(GpuChannel channel, float animTime, float* times, float4* values)
{
    const uint valueBase = channel.valuesOffset / 16u;

    if (channel.timesCount <= 1u || channel.interpolation == 1u)
    {
        return values[valueBase];
    }

    Bracket bracket = FindBracket(channel.timesOffset / 4u, channel.timesCount, animTime, times);
    return lerp(values[valueBase + bracket.lo], values[valueBase + bracket.hi], bracket.alpha);
}

float4 SampleRotation(GpuChannel channel, float animTime, float* times, float4* values)
{
    const uint valueBase = channel.valuesOffset / 16u;
    if (channel.timesCount <= 1u || channel.interpolation == 1u)
    {
        return values[valueBase];
    }
    Bracket bracket = FindBracket(channel.timesOffset / 4u, channel.timesCount, animTime, times);
    return Nlerp(values[valueBase + bracket.lo], values[valueBase + bracket.hi], bracket.alpha);
}

// Samples every channel of `clipIndex` at `animTime` into the poses. weight 1 overwrites; below 1
// the sample is blended over what is already there (a crossfade's outgoing clip over the new one).
// Nodes a clip does not animate keep what is there: the bind pose, or the other clip's value.
void SampleClip(AnimatorSampleJob job, uint clipIndex, float animTime, float weight, SampledNodePose* poses)
{
    GpuClip clip = job.clips.Ptr()[clipIndex];
    GpuChannel* channels = job.channels.Ptr();
    float* times = job.times.Ptr();
    float4* values = job.values.Ptr();
    const bool overwrite = weight >= 1.0f;

    for (uint chIdx = 0u; chIdx < clip.channelCount; ++chIdx)
    {
        GpuChannel channel = channels[clip.channelOffset + chIdx];

        uint nodeIdx = channel.nodeIndex;
        if (nodeIdx >= job.nodeCount)
            continue;

        uint animPath = channel.animPath;
        uint poseIdx = job.nodePoseOffset + nodeIdx;

        if (animPath == 0u)  // Translation
        {
            float4 sampled = SampleChannelValue(channel, animTime, times, values);
            poses[poseIdx].translation = overwrite ? sampled : lerp(poses[poseIdx].translation, sampled, weight);
        }
        else if (animPath == 1u)  // Rotation
        {
            float4 sampled = SampleRotation(channel, animTime, times, values);
            poses[poseIdx].rotation = overwrite ? sampled : Nlerp(poses[poseIdx].rotation, sampled, weight);
        }
        else if (animPath == 2u)  // Scale
        {
            float4 sampled = SampleChannelValue(channel, animTime, times, values);
            poses[poseIdx].scale = overwrite ? sampled : lerp(poses[poseIdx].scale, sampled, weight);
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

    AnimatorSampleJob job = pc.animatorJobs.Load(jobIdx);

    if (job.animClipIndex >= job.clipCount)
        return;

    if (job.clips.addr == 0 || job.channels.addr == 0 || job.times.addr == 0 || job.values.addr == 0 || pc.sampledPoses.addr == 0)
        return;

    SampledNodePose* poses = pc.sampledPoses.Ptr();
    SampleClip(job, job.animClipIndex, job.animTime, 1.0f, poses);
    if (job.fadeWeight > 0.0f && job.fadeClipIndex < job.clipCount)
    {
        SampleClip(job, job.fadeClipIndex, job.fadeTime, job.fadeWeight, poses);
    }
}
