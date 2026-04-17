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
        return result;
    }

    for (uint i = 0; i + 1 < count; ++i)
    {
        float t0 = allTimes[timesOffset + i];
        float t1 = allTimes[timesOffset + i + 1];
        
        if (t < t1)
        {
            result.lo = i;
            result.hi = i + 1;
            float range = t1 - t0;
            result.alpha = (range > 1e-6f) ? (t - t0) / range : 0.0f;
            return result;
        }
    }

    return result;
}

// Slerp two quaternions (in xyzw format as float4).
float4 Slerp(float4 q1, float4 q2, float t)
{
    float dot_val = dot(q1, q2);

    if (dot_val < 0.0f)
    {
        q2 = -q2;
        dot_val = -dot_val;
    }

    dot_val = clamp(dot_val, -1.0f, 1.0f);

    float theta = acos(dot_val);
    float sin_theta = sin(theta);

    if (sin_theta < 1e-6f)
        return lerp(q1, q2, t);

    float w1 = sin((1.0f - t) * theta) / sin_theta;
    float w2 = sin(t * theta) / sin_theta;

    return w1 * q1 + w2 * q2;
}

// ─────────────────────────────────────────────────────────────────────────────

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint jobIdx = dtid.x;
    if (jobIdx >= pc.jobCount)
        return;

    // Load job state
    AnimatorSampleJob* jobs = (AnimatorSampleJob*)(pc.animatorJobsAddr);
    AnimatorSampleJob job = jobs[jobIdx];

    // Load animation clip metadata
    GpuClip* clips = (GpuClip*)(pc.animDbClipsAddr);
    GpuChannel* channels = (GpuChannel*)(pc.animDbChannelsAddr);
    float* times = (float*)(pc.animDbTimesAddr);
    float4* values = (float4*)(pc.animDbValuesAddr);
    float4* bindTranslations = (float4*)(pc.bindTranslationsAddr);
    float4* bindRotations = (float4*)(pc.bindRotationsAddr);
    float4* bindScales = (float4*)(pc.bindScalesAddr);
    SampledNodePose* poses = (SampledNodePose*)(pc.sampledPosesAddr);

    if (job.animClipIndex >= pc.clipCount)
        return;

    const uint poseBase = job.nodePoseOffset;
    for (uint j = 0u; j < job.nodeCount; ++j)
    {
        SampledNodePose initPose;
        initPose.translation = bindTranslations[j];
        initPose.rotation = bindRotations[j];
        initPose.scale = bindScales[j];
        poses[poseBase + j] = initPose;
    }

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
                : Slerp(values[valIdx0], values[valIdx1], bracket.alpha);
            poses[poseIdx].rotation = normalize(sampled);
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
