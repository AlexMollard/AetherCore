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
    VSOutput o;
    o.position = float4(c * 2.0 - 1.0, 0.0, 1.0);
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

    // Pixel-snap to a coarse world grid (16 px/unit) so the ink reads as chunky pixel-art.
    const float cell = (1.0 / 16.0) * 2.0;
    const float2 wq = (floor(world / cell) + 0.5) * cell;

    InkSegment* segs = pc.data.Ptr();
    const uint segmentCount = pc.count / 2u; // 2 float4 per InkSegment
    float best = 1e9;
    float bAlpha = 0.0, bGlow = 0.0, bGhost = 0.0;
    for (uint i = 0u; i < segmentCount; ++i)
    {
        const InkSegment s = segs[i];
        // Roughen the boundary with fbm so the edge looks brushed, not machined.
        const float n = fbm(wq * 6.5 + s.a * 3.1) - 0.5;
        const float d = (sdSegment(wq, s.a, s.b) - s.width) + n * 0.07;
        if (d < best)
        {
            best = d;
            bAlpha = s.alpha;
            bGlow = s.glow;
            bGhost = s.ghost;
        }
    }

    const float aa = cell; // ~one pixel-cell soft edge
    if (best > aa * 1.5)
    {
        discard;
    }

    const float time = frame->elapsedTime;
    const float wet = 0.5 + 0.5 * sin(time * 2.0 + wq.x * 4.0 + wq.y * 3.0);

    float3 body = pc.color0.rgb;
    float3 rim = pc.color1.rgb;
    if (bGhost > 0.5)
    {
        body = float3(0.34, 0.05, 0.06);
        rim = float3(1.0, 0.30, 0.28);
    }

    // Rim glow that rides the wet edge (best in [-rimWidth, 0]).
    const float rimWidth = 0.11;
    const float rimT = 1.0 - saturate(-best / rimWidth);
    float3 col = lerp(body, rim, rimT * rimT * (0.55 + 0.45 * wet) * bGlow);

    // A little inner grain so the body isn't a flat fill.
    col *= 0.9 + 0.1 * floor(vnoise(wq * 9.0 + time * 0.1) * 3.0) / 3.0;

    const float fill = 1.0 - smoothstep(-aa, aa, best);
    return float4(col, fill * bAlpha);
}
