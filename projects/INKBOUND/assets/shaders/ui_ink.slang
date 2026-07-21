// Liquid ink transition - a custom UI effect shader (UIEffect component, "ui_ink").
// Project-owned game shader: compiled on load to shaders://ui_ink.spv and drawn by UiRenderer's
// effect pass with its own pipeline. Ink rises from the bottom (coverage 0..1) with a layered,
// tendrilled, paper-grained liquid boundary and a bright rim.

// Bindless set 0 declared (unused) so the pipeline layout matches the bound descriptor heap.
[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

struct InkPush
{
    float4 screenSize; // .xy = viewport dimensions
    float4 rect;       // x, y, w, h in px
    float4 params;     // coverage, time, noiseAmp, edgeWidth
    float4 inkColor;   // ink body RGBA
    float4 edgeColor;  // rim RGBA
};

[[vk::push_constant]] InkPush pc;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0; // 0..1 across the rect; uv.y 0 = top, 1 = bottom
};

[shader("vertex")]
VSOutput vertexMain(uint vertexId : SV_VertexID)
{
    const float2 uvRect[6] = {
        float2(0.0, 1.0), float2(1.0, 1.0), float2(1.0, 0.0),
        float2(0.0, 1.0), float2(1.0, 0.0), float2(0.0, 0.0),
    };
    const float2 uv = uvRect[vertexId % 6u];
    const float2 sp = pc.rect.xy + uv * pc.rect.zw;

    VSOutput o;
    o.position = float4((sp.x / pc.screenSize.x) * 2.0 - 1.0, (sp.y / pc.screenSize.y) * 2.0 - 1.0, 0.0, 1.0);
    o.uv = uv;
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
    const float a = hash21(i);
    const float b = hash21(i + float2(1.0, 0.0));
    const float c = hash21(i + float2(0.0, 1.0));
    const float d = hash21(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float fbm(float2 p)
{
    float v = 0.0;
    float a = 0.5;
    [unroll]
    for (int k = 0; k < 4; ++k)
    {
        v += a * vnoise(p);
        p *= 2.0;
        a *= 0.5;
    }
    return v;
}

// Pixel-art helpers, matched to ui_void_bg so the whole menu shares one virtual-pixel scale.
static const float kPixelsWide = 300.0;

float2 Pixelate(float2 uv, float aspect)
{
    const float2 res = float2(kPixelsWide, max(kPixelsWide / max(aspect, 0.001), 1.0));
    return (floor(uv * res) + 0.5) / res;
}

// Surface uv.y for a boundary, given its own wave + drip params. Lower value = reaches higher.
float inkBoundary(float uvx, float surf, float amp, float time, float win, float xf, float tf, float dripMul, float dripSharp)
{
    const float wave = (fbm(float2(uvx * xf, time * tf)) - 0.5) * 2.0 * amp;
    const float drip = pow(saturate(fbm(float2(uvx * (xf * 2.6) + 5.3, time * (tf * 0.6)))), dripSharp);
    return surf + (wave - drip * amp * dripMul) * win;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    const float coverage = pc.params.x;
    const float time     = pc.params.y;
    const float amp      = pc.params.z;
    const float edgeW    = max(pc.params.w, 0.001);

    // Snap to the same coarse virtual-pixel grid as the backdrop so the ink boundary stair-steps
    // in chunky pixels instead of a smooth curve.
    const float aspect = pc.rect.z / max(pc.rect.w, 1.0);
    const float2 uv = Pixelate(input.uv, aspect);
    const float uvx = uv.x;
    const float uvy = uv.y;

    const float win = smoothstep(0.0, 0.12, coverage) * (1.0 - smoothstep(0.88, 1.0, coverage));
    const float surf = (1.0 - coverage) - 0.02 * coverage; // over-fill scales in as it nears full

    // Back layer: smoother, slower, sits a touch higher for depth. Front layer: faster with long,
    // sharp dripping tendrils.
    const float boundaryB = inkBoundary(uvx, surf - 0.02 * win, amp * 1.25, time, win, 3.2, 0.35, 1.6, 3.0);
    const float boundaryF = inkBoundary(uvx, surf, amp, time, win, 5.5, 0.55, 4.0, 5.0);

    // Combined leading edge (topmost ink) from the two tendrilled boundaries; hard, un-antialiased
    // so the pixel grid does the stair-stepping. Everything above the surface is clear.
    const float lead = min(boundaryB, boundaryF);
    const float depth = uvy - lead; // 0 at the leading edge, grows downward into the body
    if (depth < 0.0)
    {
        discard;
    }

    // Banded internal folds - the same roiling-fbm structure the backdrop uses, so the ink body
    // has depth instead of reading as flat black. Back-only region (front sheet not yet here) dips
    // a shade darker for a two-layer feel.
    const float tex = fbm(float2(uvx * 5.5 + 2.3, uvy * 4.5 - time * 0.25));
    const float backOnly = 1.0 - step(boundaryF, uvy);

    // Tone 0..1: a bright crest right at the leading edge easing into the darker textured body.
    const float tone = saturate((1.0 - smoothstep(0.0, edgeW * 3.0, depth)) * 0.85
                                + tex * 0.5 - 0.12
                                - backOnly * 0.12);

    // Limited 5-colour ink palette, deep ink -> bright cyan crest - the same ramp language as
    // ui_void_bg, so the transition matches the backdrop's flat pixel-art quality.
    const float3 c0 = pc.inkColor.rgb * 0.8;                              // deepest trough
    const float3 c1 = pc.inkColor.rgb * 2.4;                              // dark body
    const float3 c2 = pc.inkColor.rgb * 4.2;                              // lifted fold
    const float3 c3 = lerp(pc.inkColor.rgb * 5.0, pc.edgeColor.rgb, 0.55); // near-crest blue-cyan
    const float3 c4 = pc.edgeColor.rgb;                                   // cyan crest
    float3 rgb = c0;
    rgb = tone >= 0.2 ? c1 : rgb;
    rgb = tone >= 0.4 ? c2 : rgb;
    rgb = tone >= 0.6 ? c3 : rgb;
    rgb = tone >= 0.8 ? c4 : rgb;

    return float4(rgb, pc.inkColor.a);
}
