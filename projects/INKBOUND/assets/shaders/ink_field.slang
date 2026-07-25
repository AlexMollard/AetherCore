// INKBOUND's conjured-ink field - a project custom pass (CustomPass, stage OverScene). A single
// fullscreen draw SDF-unions every live ink capsule (world-space segment a->b with a half-thickness)
// into a continuous wet-ink layer: dark inky body, glowing cyan rim, organic fbm-noised edges,
// pixel-snapped to match the game's pixel-art, with a subtle animated wetness. Unanchored ("ghost")
// ink shades red.
//
// Uses the engine's generic CustomPass push contract: `data` is a raw float4 buffer this shader
// reinterprets as InkSegments (2 float4 each), `count` is the float4 element count, color0/color1
// are the ink body/rim. The engine knows nothing about "ink"; the packing lives in AetherInk.cs.
#include "include/DevicePointer.slangh"
#include "include/FrameConstants.slangh"

struct InkSegment
{
    float2 a;
    float2 b;
    float  width;
    float  alpha;
    float  glow;
    float  ghost;
};

struct CustomPassPush
{
    DevicePtr<FrameConstantsData> frameConstants;
    DevicePtr<InkSegment> data; // the float4 buffer, read as 32-byte InkSegments (2 float4 each)
    uint   count;               // number of float4 elements (segmentCount = count / 2)
    uint   pad;
    float2 viewportSize;
    float4 params;
    float4 color0;              // ink body
    float4 color1;              // ink rim
};

[[vk::push_constant]] CustomPassPush pc;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

[shader("vertex")]
VSOutput vertexMain(uint vertexId : SV_VertexID)
{
    const float2 corners[6] = {
        float2(0, 0), float2(1, 0), float2(1, 1),
        float2(0, 0), float2(1, 1), float2(0, 1),
    };
    const float2 c = corners[vertexId % 6u];

    // Bound the quad to the ink's world-space AABB (params = world min.xy, max.xy) so the fullscreen
    // SDF loop only runs over pixels near actual ink, not the whole screen. Degenerate (collapsed)
    // when there is no ink, so nothing is rasterised.
    FrameConstantsData* frame = pc.frameConstants.Ptr();
    const float2 mn = pc.params.xy;
    const float2 mx = pc.params.zw;
    VSOutput o;
    if (mx.x <= mn.x || mx.y <= mn.y)
    {
        o.position = float4(0.0, 0.0, 2.0, 1.0); // behind the near plane -> clipped
        o.uv = c;
        return o;
    }
    const float2 world = lerp(mn, mx, c);
    o.position = mul(frame->viewProj, float4(world, 0.0, 1.0));
    o.uv = c;
    return o;
}

float hash21(float2 p)
{
    p = frac(p * float2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return frac(p.x * p.y);
}

float vnoise(float2 p)
{
    const float2 i = floor(p);
    const float2 f = frac(p);
    const float2 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(hash21(i), hash21(i + float2(1, 0)), u.x),
                lerp(hash21(i + float2(0, 1)), hash21(i + float2(1, 1)), u.x), u.y);
}

float fbm(float2 p)
{
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 4; ++i)
    {
        s += a * vnoise(p);
        p *= 2.03;
        a *= 0.5;
    }
    return s;
}

// Distance from p to segment a-b (2D).
float sdSegment(float2 p, float2 a, float2 b)
{
    const float2 pa = p - a;
    const float2 ba = b - a;
    const float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-6), 0.0, 1.0);
    return length(pa - ba * h);
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    FrameConstantsData* frame = pc.frameConstants.Ptr();

    // Reconstruct the world-space point under this fragment (2D ortho: world.xy depends only on
    // the screen ndc, not on the reconstruction depth).
    const float2 ndc = input.position.xy / max(pc.viewportSize, float2(1.0, 1.0)) * 2.0 - 1.0;
    const float4 wp = mul(frame->invViewProj, float4(ndc, 0.0, 1.0));
    const float2 world = wp.xy / wp.w;

    // Pixel-art: snap the sample to the game's ART grid (16 px/unit, same as the sprites/tiles) and
    // shade with HARD edges - no anti-aliasing - so the ink renders as crisp uniform pixels that match
    // the pixel art, not a muddy low-res brush. Everything below is evaluated at wq, so ink, veins and
    // glow are all per-pixel-cell constant (blocky), world-locked like the sprites' nearest sampling.
    const float P = 1.0 / 16.0;
    const float2 wq = (floor(world / P) + 0.5) * P;

    InkSegment* segs = pc.data.Ptr();
    const uint segmentCount = pc.count / 2u; // 2 float4 per InkSegment
    float best = 1e9;
    float bAlpha = 0.0, bGlow = 0.0, bGhost = 0.0;
    for (uint i = 0u; i < segmentCount; ++i)
    {
        const InkSegment s = segs[i];
        const float d = sdSegment(wq, s.a, s.b) - s.width;
        if (d < best)
        {
            best = d;
            bAlpha = s.alpha;
            bGlow = s.glow;
            bGhost = s.ghost;
        }
    }
    // One fbm per cell wobbles the boundary so the pixel edge looks brushed, not machined.
    best += (fbm(wq * 6.5) - 0.5) * 0.06;

    const float glowRadius = 0.26; // pixel glow halo reach beyond the ink surface
    if (best > glowRadius)
    {
        discard;
    }

    const float time = frame->elapsedTime;

    // Hard pixel coverage: fully opaque inside, nothing past the surface (crisp blocks, no AA).
    const float inkMask = best < 0.0 ? 1.0 : 0.0;

    // Slow inky turbulence for pooling + veins (evaluated per cell -> blocky, matches the pixel look).
    const float turb = fbm(wq * 2.8 + float2(time * 0.04, -time * 0.03));
    const float depth = saturate(-best / 0.5); // 0 at the edge -> ~1 deep inside the ink

    float3 body = pc.color0.rgb;
    float3 rim = pc.color1.rgb;
    if (bGhost > 0.5)
    {
        body = float3(0.30, 0.04, 0.05);
        rim = float3(0.92, 0.22, 0.22);
    }
    else if (bGhost < -0.5)
    {
        // Set ink: the stuff a dead wanderer left behind. Dull stone with a cold mineral edge -
        // clearly part of the cave now, and clearly not something you conjured a moment ago.
        body = float3(0.085, 0.10, 0.135);
        rim = float3(0.22, 0.30, 0.38);
        bGlow = 0.25;
    }

    // Murky pooling + veins inside the body.
    float3 ink = lerp(body, body * 0.30, depth * (0.35 + 0.65 * turb));
    ink *= 1.0 - 0.30 * (smoothstep(0.44, 0.5, turb) - smoothstep(0.5, 0.58, turb));
    // Wet meniscus sheen on the outermost pixel ring.
    const float flick = 0.72 + 0.28 * sin(time * 1.1 + wq.x * 1.7 + wq.y * 2.1);
    const float rimWidth = 0.08;
    const float rimT = 1.0 - saturate(-best / rimWidth);
    ink = lerp(ink, rim, rimT * rimT * 0.5 * bGlow * flick);

    // Pixel glow halo OUTSIDE the ink, quantised into a few steps so it reads as blocky pixel glow.
    float halo = 1.0 - saturate(max(best, 0.0) / glowRadius);
    halo = floor(halo * halo * 4.0 + 0.5) / 4.0;
    const float pulse = 0.82 + 0.18 * sin(time * 1.6);
    const float glowA = halo * (1.0 - inkMask) * 0.55 * bGlow * pulse;
    const float3 glowCol = rim * 1.5;

    // Composite the opaque ink over its glow halo (straight alpha).
    const float inkA = inkMask * bAlpha;
    const float a = saturate(inkA + glowA);
    const float3 col = (ink * inkA + glowCol * glowA) / max(a, 1e-4);
    return float4(col, a);
}
