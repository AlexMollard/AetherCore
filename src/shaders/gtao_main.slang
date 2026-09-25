// Half-resolution ground-truth ambient occlusion.
// Writes one R8 value per half-res pixel; depth is sampled from the predepth pass.

[[vk::binding(0, 0)]] Texture2D g_textures[];

#include "include/FrameConstants.slangh"
#include "include/FullScreenTriangle.slangh"
#include "include/OctNormal.slangh"

struct GTAOPush
{
    uint fullWidth;
    uint fullHeight;
    uint frameIndex;
    uint64_t frameConstantsAddr;
    float radius;
    float strength;
    uint enabled;
    uint _pad1;
};

[[vk::push_constant]] GTAOPush pc;

static float Hash(uint s)
{
    s = s * 747796405u + 2891336453u;
    s = ((s >> 16u) ^ s) * 0x45d9f3bu;
    s = ((s >> 16u) ^ s) * 0x45d9f3bu;
    s = (s >> 16u) ^ s;
    return float(s) * (1.0f / 4294967296.0f);
}

static float2 Hash2(uint2 px)
{
    return float2(Hash(px.x + px.y * 65537u), Hash(px.x * 65537u + px.y + 17u));
}

static uint2 ClampFull(int2 p)
{
    return uint2(
        uint(clamp(p.x, 0, int(pc.fullWidth) - 1)),
        uint(clamp(p.y, 0, int(pc.fullHeight) - 1)));
}

static float LoadDepth(uint2 px, FrameConstantsData* fc)
{
    ResourceEntry depthEntry = LoadResourceEntry(fc->resourceTableAddr, kFrameResourceId_SceneDepth);
    if (depthEntry.type != kResourceTypeBindlessTexture || depthEntry.address == 0xFFFFFFFFu || depthEntry.width == 0u || depthEntry.height == 0u)
    {
        return 1.0f;
    }
    return g_textures[uint(depthEntry.address)].Load(int3(px, 0)).r;
}

static float3 ReconstructWorld(uint2 px, float depth, FrameConstantsData* fc)
{
    float2 uv = (float2(px) + 0.5f) / float2(pc.fullWidth, pc.fullHeight);
    float2 ndc = uv * 2.0f - 1.0f;
    float4 clipPos = float4(ndc, depth, 1.0f);
    float4 worldPos = mul(fc->invViewProj, clipPos);
    return worldPos.xyz / max(worldPos.w, 1e-6f);
}

// The prepass writes the real surface normal, so prefer it. Reconstructing from
// neighbouring depth samples is only ever an estimate of the plane through them:
// across a silhouette the neighbours belong to different surfaces entirely, and
// the normal comes out somewhere between the two, which is why occlusion softens
// exactly where geometry meets. Falls back to the estimate when no G-buffer is
// bound, so this still works if the prepass is ever skipped.
static bool LoadGBufferNormal(uint2 px, FrameConstantsData* fc, out float3 normal)
{
    normal = float3(0.0f, 1.0f, 0.0f);
    ResourceEntry e = LoadResourceEntry(fc->resourceTableAddr, kFrameResourceId_SceneGBuffer);
    if (e.type != kResourceTypeBindlessTexture || e.address == 0xFFFFFFFFu || e.width == 0u || e.height == 0u)
    {
        return false;
    }
    const float2 oct = g_textures[uint(e.address)].Load(int3(px, 0)).rg;
    normal = DecodeOctNormal(oct);
    return true;
}

static float3 NormalFromDepth(uint2 px, float3 centerWorld, FrameConstantsData* fc)
{
    uint2 l = ClampFull(int2(px) + int2(-1, 0));
    uint2 r = ClampFull(int2(px) + int2(1, 0));
    uint2 u = ClampFull(int2(px) + int2(0, -1));
    uint2 d = ClampFull(int2(px) + int2(0, 1));

    float3 wl = ReconstructWorld(l, LoadDepth(l, fc), fc);
    float3 wr = ReconstructWorld(r, LoadDepth(r, fc), fc);
    float3 wu = ReconstructWorld(u, LoadDepth(u, fc), fc);
    float3 wd = ReconstructWorld(d, LoadDepth(d, fc), fc);

    float3 dxL = centerWorld - wl;
    float3 dxR = wr - centerWorld;
    float3 dyU = centerWorld - wu;
    float3 dyD = wd - centerWorld;

    float3 dx = dot(dxL, dxL) < dot(dxR, dxR) ? dxL : dxR;
    float3 dy = dot(dyU, dyU) < dot(dyD, dyD) ? dyU : dyD;

    float3 n = cross(dx, dy);
    float nLen2 = dot(n, n);
    if (nLen2 <= 1e-12f)
    {
        return normalize(fc->cameraWorldPos.xyz - centerWorld);
    }
    n *= rsqrt(nLen2);

    if (dot(n, fc->cameraWorldPos.xyz - centerWorld) < 0.0f)
    {
        n = -n;
    }
    return n;
}

// Ground-truth ambient occlusion, as Jimenez et al. define it: sweep a set of slices
// through the view vector, find the horizon angle each side of every slice reaches, and
// integrate the cosine-weighted visible arc between them analytically.
//
// The distinction that matters is what counts as an occluder. Asking whether a sample
// sits above the receiver's tangent plane - the previous test - misses the single most
// important case in any scene: a wall rising beside a floor is displaced almost purely
// sideways, so it never registers, and wall/floor junctions came out with no occlusion
// at all. A horizon angle is measured against the view vector instead, so a surface
// blocking the sky is found wherever it stands.
static float ComputeAO(uint2 px, float3 worldPos, float3 normal, FrameConstantsData* fc)
{
    static const uint kSliceCount = 4u;
    static const uint kStepCount = 6u;
    static const float kPi = 3.14159265f;
    static const float kHalfPi = 1.57079633f;

    // Occlusion fades out over this outer fraction of the radius and is gone AT the radius
    // (XeGTAO's falloff range). Fading only beyond the radius, as this did before, never
    // faded anything: the kernel only samples out to the radius, so every occluder inside it
    // counted in full, however far from the receiver.
    static const float kFalloffRange = 0.6f;

    // Screen space sees only an occluder's front face, so everything reads as infinitely
    // thick: a character's arm half a metre in front of the ground occluded that ground like
    // a wall, drawing a dark band around the whole silhouette. Stretching each sample's
    // offset along the view axis before the falloff (XeGTAO's thin-occluder compensation)
    // lets depth-separated occluders fade sooner, while contact creases - whose samples are
    // offset mostly across the screen - keep their darkening.
    static const float kThinOccluderCompensation = 0.7f;

    // Minimum elevation, as a sine, that a sample must clear above the receiver's tangent
    // plane before it is allowed to occlude - a touch under half a degree. Guards against a
    // flat surface shadowing itself at grazing angles; see the note at its use below.
    //
    // A numerical guard, not a look knob: swept from 0.46 to 3 degrees, the flat-surface
    // readings were IDENTICAL to the last value, so nothing in that range trades artifact
    // against correctness. Only the creases move, and they keep more of their darkening the
    // smaller it is, so it sits just high enough to clear depth quantisation.
    static const float kSinBias = 0.008f;

    float3 viewPos = mul(fc->view, float4(worldPos, 1.0f)).xyz;
    float3 viewNormal = normalize(mul((float3x3)fc->view, normal));
    float3 V = normalize(-viewPos);
    float viewDepth = max(-viewPos.z, 1e-4f);

    // World radius projected to pixels, so the kernel covers a fixed slice of the world
    // rather than a fixed slice of the screen.
    //
    // abs(), because proj[1][1] is NEGATIVE: Camera.cpp flips it to put Vulkan's Y axis the
    // right way up. Without that the whole expression came out negative and clamp() pinned
    // the search to its 2-pixel floor at every depth, on every surface. The effect was not
    // subtle - occlusion could only ever be found two pixels away, so the buffer held a
    // one-pixel line wherever there was a depth discontinuity and nothing else at all.
    // It also explains why the feature appeared insensitive to its own settings: raising
    // gtaoRadius from 1.4 to 3.0 changed nothing, because the result was clamped either way.
    float pixelRadius = clamp(pc.radius * abs(fc->proj[1][1]) * 0.5f * float(pc.fullHeight) / viewDepth, 2.0f, 192.0f);

    // Not white noise. This renderer has no temporal accumulation to average the sampling
    // error away over frames, so the spatial denoise below is the ONLY thing that gets to
    // remove it - and a white-noise hash spreads its error across every frequency, including
    // the low ones a blur of any practical radius cannot touch. What survives is the clumpy
    // grain that shows up in creases.
    //
    // A low-discrepancy pattern puts almost all of its error at high frequency instead, in a
    // form the bilateral filter removes nearly completely, for the same sample count. The
    // rotation uses interleaved gradient noise (Jimenez) and the step offset a 4x4 ordered
    // pattern, chosen to decorrelate from it so the two do not line up into a visible weave.
    const float ign = frac(52.9829189f * frac(0.06711056f * px.x + 0.00583715f * px.y));
    const uint2 ipx = uint2(px);
    const float ordered = (1.0f / 16.0f) * float((((ipx.y - ipx.x) & 3u) << 2u) + (ipx.x & 3u));
    float2 noise = float2(ign, ordered);
    float visibility = 0.0f;

    [unroll]
    for (uint slice = 0u; slice < kSliceCount; ++slice)
    {
        float phi = (float(slice) + noise.x) * (kPi / float(kSliceCount));
        float2 omega = float2(cos(phi), sin(phi));

        // The plane this slice sweeps, spanned by the screen direction and the view
        // vector, rotating about `axis`.
        float3 sliceDir = float3(omega, 0.0f);
        float3 axis = cross(sliceDir, V);
        float axisLen = length(axis);
        if (axisLen < 1e-4f)
        {
            continue;
        }
        axis /= axisLen;

        // The normal projected into that plane. Its length weights the slice, because a
        // slice seen edge-on to the normal covers little of the hemisphere.
        float3 projNormal = viewNormal - axis * dot(viewNormal, axis);
        float projLen = length(projNormal);
        if (projLen < 1e-4f)
        {
            continue;
        }
        float3 projNormalN = projNormal / projLen;

        // Signed angle from the view vector to the projected normal, inside the slice
        // plane. Every horizon below is measured relative to this.
        float3 tangent = cross(axis, V);
        float gamma = -sign(dot(projNormalN, tangent)) * acos(clamp(dot(projNormalN, V), -1.0f, 1.0f));

        // Highest horizon each half of the slice reaches. -1 is "nothing found", i.e.
        // open all the way to the far horizon.
        float cosH1 = -1.0f;
        float cosH2 = -1.0f;

        [unroll]
        for (uint step = 1u; step <= kStepCount; ++step)
        {
            float offsetPx = max(((float(step) - noise.y) / float(kStepCount)) * pixelRadius, 1.0f);

            [unroll]
            for (uint side = 0u; side < 2u; ++side)
            {
                float2 dirPx = (side == 0u) ? omega : -omega;
                uint2 samplePx = ClampFull(int2(float2(px) + dirPx * offsetPx));
                float sampleDepth = LoadDepth(samplePx, fc);
                if (sampleDepth >= 1.0f)
                {
                    // Sky: no horizon here, leave this side open.
                    continue;
                }

                float3 sampleView = mul(fc->view, float4(ReconstructWorld(samplePx, sampleDepth, fc), 1.0f)).xyz;
                float3 delta = sampleView - viewPos;
                float len = length(delta);
                if (len < 1e-4f)
                {
                    continue;
                }

                // A surface does not occlude itself. Seen edge-on, a flat surface's own
                // neighbours are displaced almost entirely ALONG the view ray, so each one
                // reports a horizon sitting right at the view vector - the strongest
                // occlusion the integral can express. Depth quantisation then scatters
                // those co-planar samples either side of the tangent plane, and because the
                // horizon is accumulated with max(), the scatter is rectified: only the
                // samples that landed on the occluding side survive. Noise becomes bias.
                //
                // Measured on flat, entirely unoccluded walls before this test existed: a
                // back wall fell from 255 to 157 across its width as the view grazed it, and
                // a wall seen almost edge-on sat at 48/255 - 80% occlusion - over its whole
                // face, with no geometry near it at all.
                //
                // So a sample has to rise clear of the receiver's own tangent plane before
                // it counts - and the margin is an ANGLE, not a distance. Measuring it as a
                // fixed world offset instead rejects every sample within that offset, which
                // is precisely the contact crease at the foot of a wall or box: those
                // samples are close, so their height above the plane is small even though
                // they rise almost vertically. Tried that first and it erased the contact
                // darkening along with the artifact, leaving an almost uniformly white
                // buffer. Dividing by the sample's own distance makes the test scale-free:
                // a co-planar neighbour sits at zero elevation however far away it is, while
                // anything genuinely standing up off the surface clears the threshold at any
                // distance.
                const float sinElevation = dot(delta, viewNormal) / len;
                if (sinElevation < kSinBias)
                {
                    continue;
                }

                float cosH = dot(delta / len, V);

                // Fade the horizon back toward open as the occluder nears the edge of the
                // radius, so it leaves the kernel without popping, with the view-axis offset
                // stretched so thin foreground occluders fade first.
                const float falloffLen = length(float3(delta.xy, delta.z * (1.0f + kThinOccluderCompensation)));
                const float falloff = saturate((pc.radius - falloffLen) / (pc.radius * kFalloffRange));
                cosH = lerp(-1.0f, cosH, falloff);

                if (side == 0u)
                {
                    cosH1 = max(cosH1, cosH);
                }
                else
                {
                    cosH2 = max(cosH2, cosH);
                }
            }
        }

        // Horizons as signed angles either side of the view vector, then clamped into
        // the hemisphere about the normal - past that a surface only occludes itself.
        float h1 = -acos(clamp(cosH1, -1.0f, 1.0f));
        float h2 = acos(clamp(cosH2, -1.0f, 1.0f));
        h1 = gamma + max(h1 - gamma, -kHalfPi);
        h2 = gamma + min(h2 - gamma, kHalfPi);

        // Analytic cosine-weighted arc between the two horizons.
        float sinGamma = sin(gamma);
        float cosGamma = cos(gamma);
        float arc1 = -cos(2.0f * h1 - gamma) + cosGamma + 2.0f * h1 * sinGamma;
        float arc2 = -cos(2.0f * h2 - gamma) + cosGamma + 2.0f * h2 * sinGamma;
        visibility += projLen * 0.25f * (arc1 + arc2);
    }

    float ao = saturate(visibility / float(kSliceCount));

    // Low floor: creases and contact points are allowed to get genuinely dark.
    return max(pow(ao, pc.strength), 0.05f);
}

[shader("fragment")]
float4 fragmentMain(float4 svPos : SV_Position) : SV_Target0
{
    uint2 halfPx = uint2(svPos.xy);
    FrameConstantsData* fc = (FrameConstantsData*)pc.frameConstantsAddr;

    // Ambient occlusion off: still write the target rather than leaving the draw out.
    // The graph publishes this image as a frame product whatever happens in here, so a
    // pass that writes nothing leaves every shader reading a recycled transient - which
    // is not white, and made switching AO OFF darken the scene instead of brightening
    // it. Returning here skips all the sampling; only the write remains.
    if (pc.enabled == 0u)
    {
        return float4(1.0f, 0.0f, 0.0f, 1.0f);
    }

    if (pc.fullWidth == 0u || pc.fullHeight == 0u)
    {
        return float4(1.0f, 0.0f, 0.0f, 1.0f);
    }

    uint2 fullPx = ClampFull(int2(halfPx * 2u));
    float depth = LoadDepth(fullPx, fc);
    if (depth >= 1.0f)
    {
        return float4(1.0f, 0.0f, 0.0f, 1.0f);
    }

    float3 worldPos = ReconstructWorld(fullPx, depth, fc);
    float3 normal;
    if (!LoadGBufferNormal(fullPx, fc, normal))
    {
        normal = NormalFromDepth(fullPx, worldPos, fc);
    }
    float ao = ComputeAO(fullPx, worldPos, normal, fc);
    return float4(ao, 0.0f, 0.0f, 1.0f);
}
