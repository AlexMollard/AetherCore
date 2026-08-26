// Half-resolution ground-truth ambient occlusion.
// Writes one R8 value per half-res pixel; depth is sampled from the predepth pass.

[[vk::binding(0, 0)]] Texture2D g_textures[];

#include "include/FrameConstants.slangh"
#include "include/FullScreenTriangle.slangh"

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

    // How far behind the receiver an occluder may sit, along the view ray, before it
    // reads as a separate surface rather than a fold of this one. Without it a distant
    // wall behind a foreground object darkens that object's silhouette.
    static const float kThickness = 1.0f;

    float3 viewPos = mul(fc->view, float4(worldPos, 1.0f)).xyz;
    float3 viewNormal = normalize(mul((float3x3)fc->view, normal));
    float3 V = normalize(-viewPos);
    float viewDepth = max(-viewPos.z, 1e-4f);

    // World radius projected to pixels, so the kernel covers a fixed slice of the world
    // rather than a fixed slice of the screen.
    float pixelRadius = clamp(pc.radius * fc->proj[1][1] * 0.5f * float(pc.fullHeight) / viewDepth, 2.0f, 192.0f);

    float2 noise = Hash2(px);
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

                float cosH = dot(delta / len, V);

                // Ease the horizon back toward open past the radius, so an occluder
                // leaving the kernel fades out instead of popping, and drop occluders
                // that sit far enough behind to be a different surface entirely.
                float falloff = saturate(1.0f - (len - pc.radius) / kThickness);
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
    float3 normal = NormalFromDepth(fullPx, worldPos, fc);
    float ao = ComputeAO(fullPx, worldPos, normal, fc);
    return float4(ao, 0.0f, 0.0f, 1.0f);
}
