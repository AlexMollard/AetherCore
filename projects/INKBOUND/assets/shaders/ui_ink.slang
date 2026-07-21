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
    const float uvx = input.uv.x;
    const float uvy = input.uv.y;

    const float win = smoothstep(0.0, 0.12, coverage) * (1.0 - smoothstep(0.88, 1.0, coverage));
    const float surf = (1.0 - coverage) - 0.02 * coverage; // over-fill scales in as it nears full

    // Back layer: smoother, slower, sits a touch higher for depth. Front layer: faster with long,
    // sharp dripping tendrils.
    const float boundaryB = inkBoundary(uvx, surf - 0.02 * win, amp * 1.25, time, win, 3.2, 0.35, 1.6, 3.0);
    const float boundaryF = inkBoundary(uvx, surf, amp, time, win, 5.5, 0.55, 4.0, 5.0);

    const float aa = fwidth(uvy) * 1.5;
    const float inkB = smoothstep(-aa, aa, uvy - boundaryB);
    const float inkF = smoothstep(-aa, aa, uvy - boundaryF);
    const float ink = max(inkB, inkF);
    if (ink < 0.004)
    {
        discard;
    }

    // Fine paper grain in the ink body (subtle brightness break-up).
    const float grain = (vnoise(input.uv * float2(190.0, 120.0)) - 0.5) * 0.10;

    // Two-tone body: the back-only region reads a shade darker than where the front sheet lies.
    float3 body = pc.inkColor.rgb * (1.0 + grain);
    body *= lerp(0.78, 1.05, inkF);

    const float rim = smoothstep(edgeW, 0.0, abs(uvy - boundaryF)); // rim on the leading (front) edge
    const float3 rgb = lerp(body, pc.edgeColor.rgb, rim * pc.edgeColor.a);
    const float alpha = max(pc.inkColor.a * ink, rim * pc.edgeColor.a);
    return float4(rgb, alpha);
}
