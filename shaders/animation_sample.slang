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

#include "include/BindlessContract.slangh"
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

// ─────────────────────────────────────────────────────────────────────────────

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint jobIdx = dtid.x;
    if (jobIdx >= pc.jobCount)
        return;

    AnimatorSampleJob* jobs = (AnimatorSampleJob*)(pc.animatorJobsAddr);
    AnimatorSampleJob job = jobs[jobIdx];

    if (job.animClipIndex >= job.clipCount)
        return;

    GpuClip* clips = (GpuClip*)(job.clipsAddr);
    GpuChannel* channels = (GpuChannel*)(job.channelsAddr);
    float* times = (float*)(job.timesAddr);
    float4* values = (float4*)(job.valuesAddr);
    SampledNodePose* poses = (SampledNodePose*)(pc.sampledPosesAddr);

    const uint poseBase = job.nodePoseOffset;

    GpuClip clip = clips[job.animClipIndex];

    // Sample all channels in this clip
    for (uint chIdx = 0u; chIdx < clip.channelCount; ++chIdx)
    {
        uint globalChIdx = clip.channelOffset + chIdx;
        GpuChannel channel = channels[globalChIdx];

        uint nodeIdx = channel.nodeIndex;
        if (nodeIdx >= job.nodeCount)
            continue;

        // Sample keyframes based on channel path type
        uint animPath = channel.animPath;
        uint poseIdx = poseBase + nodeIdx;

        if (animPath == 0u)  // Translation
        {
            // Linear interpolation for translation
            Bracket bracket = FindBracket(channel.timesOffset / 4u, channel.timesCount, job.animTime, times);
            uint valIdx0 = channel.valuesOffset / 16u + bracket.lo;
            uint valIdx1 = channel.valuesOffset / 16u + bracket.hi;
            float a = (channel.interpolation == 1u) ? 0.0f : bracket.alpha;
            float4 sampled = lerp(values[valIdx0], values[valIdx1], a);
            poses[poseIdx].translation = sampled;
        }
        else if (animPath == 1u)  // Rotation
        {
            // Slerp for rotations
            Bracket bracket = FindBracket(channel.timesOffset / 4u, channel.timesCount, job.animTime, times);
            uint valIdx0 = channel.valuesOffset / 16u + bracket.lo;
            uint valIdx1 = channel.valuesOffset / 16u + bracket.hi;
            float4 sampled = (channel.interpolation == 1u)
                ? values[valIdx0]
                : Nlerp(values[valIdx0], values[valIdx1], bracket.alpha);
            poses[poseIdx].rotation = SafeNormalize(sampled);
        }
        else if (animPath == 2u)  // Scale
        {
            // Linear interpolation for scale
            Bracket bracket = FindBracket(channel.timesOffset / 4u, channel.timesCount, job.animTime, times);
            uint valIdx0 = channel.valuesOffset / 16u + bracket.lo;
            uint valIdx1 = channel.valuesOffset / 16u + bracket.hi;
            float a = (channel.interpolation == 1u) ? 0.0f : bracket.alpha;
            float4 sampled = lerp(values[valIdx0], values[valIdx1], a);
            poses[poseIdx].scale = sampled;
        }
    }
}
