// Liquid ink transition - a custom UI effect shader (UIEffect component, "ui_ink").
// Draws a full-screen quad over the element's rect; the ink rises from the bottom with an
// organic, wavy, tendrilled boundary and a bright rim, driven by a 0..1 coverage value.
// Rendered by UiRenderer's effect pass with its OWN pipeline - ui_shapes.slang stays untouched.

// Bindless set 0 is declared (unused here) so this pipeline's layout matches the descriptor
// heap the renderer binds; see the "bind g_textures at set 0" rule.
[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

struct InkPush
{
    float4 screenSize; // .xy = viewport dimensions
    float4 rect;       // x, y, w, h in px
    float4 params;     // coverage, time, noiseAmp, edgeWidth
    float4 inkColor;   // ink body RGBA
    float4 edgeColor;  // rim/edge RGBA
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

    const float ndcX = (sp.x / pc.screenSize.x) * 2.0 - 1.0;
    const float ndcY = (sp.y / pc.screenSize.y) * 2.0 - 1.0;

    VSOutput o;
    o.position = float4(ndcX, ndcY, 0.0, 1.0);
    o.uv = uv;
    return o;
}

// Hash-based value noise + fbm (no textures).
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

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    const float coverage = pc.params.x;
    const float time     = pc.params.y;
    const float amp      = pc.params.z;
    const float edgeW    = max(pc.params.w, 0.001);

    // Fade the surface noise to nothing at the extremes: coverage 0 is fully clear and
    // coverage 1 is fully opaque (no gaps at the very top).
    const float noiseWin = smoothstep(0.0, 0.12, coverage) * (1.0 - smoothstep(0.88, 1.0, coverage));

    // Surface line in uv.y. Ink rises from the bottom; a coverage-scaled over-fill guarantees a
    // clean full cover without leaving a seam at the top row.
    const float surf = (1.0 - coverage) - 0.015 * coverage;

    // Rolling body wave + spiky upward tendrils (ink climbing).
    const float wave    = (fbm(float2(input.uv.x * 4.5, time * 0.5)) - 0.5) * 2.0 * amp;
    const float tendril = pow(saturate(fbm(float2(input.uv.x * 13.0 + 5.3, time * 0.3))), 3.0);
    const float boundary = surf + (wave - tendril * amp * 3.0) * noiseWin;

    const float d  = input.uv.y - boundary; // >0 = ink (below surface), <0 = clear
    const float aa = fwidth(input.uv.y) * 1.5;
    const float ink = smoothstep(-aa, aa, d);
    if (ink < 0.004)
    {
        discard;
    }

    const float rim = smoothstep(edgeW, 0.0, abs(d)); // bright glow along the boundary
    const float3 rgb = lerp(pc.inkColor.rgb, pc.edgeColor.rgb, rim * pc.edgeColor.a);
    const float alpha = max(pc.inkColor.a * ink, rim * pc.edgeColor.a);
    return float4(rgb, alpha);
}
