// Menu background - a custom UI effect shader (UIEffect component, "ui_void_bg", background=true).
// A slow, roiling dark ink/void with faint drifting clouds, occasional accent wisps, and a soft
// vignette. Opaque; drawn BEHIND the batched UI. params.x = time; color0 = void base, color1 = wisp.

[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

struct BgPush
{
    float4 screenSize;
    float4 rect;
    float4 params;   // x = time, y = intensity (0..1)
    float4 color0;   // void base
    float4 color1;   // accent wisp tint
};

[[vk::push_constant]] BgPush pc;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
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
    for (int k = 0; k < 5; ++k)
    {
        v += a * vnoise(p);
        p = p * 2.0 + 11.3;
        a *= 0.5;
    }
    return v;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    const float time = pc.params.x;
    const float intensity = pc.params.y > 0.0 ? pc.params.y : 1.0;
    const float2 uv = input.uv;

    // Aspect-correct domain so clouds aren't stretched.
    const float aspect = pc.screenSize.x / max(pc.screenSize.y, 1.0);
    const float2 p = float2(uv.x * aspect, uv.y);

    // Two layers of slow roiling ink; a small domain-warp gives it a liquid, folded look.
    const float2 warp = float2(fbm(p * 2.0 + time * 0.02), fbm(p * 2.0 - time * 0.015)) - 0.5;
    const float n1 = fbm(p * 2.6 + warp * 0.8 + float2(time * 0.025, time * 0.015));
    const float n2 = fbm(p * 5.5 - float2(time * 0.02, 0.0));
    const float clouds = smoothstep(0.30, 0.78, n1 * 0.72 + n2 * 0.28);

    // Void base lifted slightly in the cloud folds; ink pools darker in the troughs.
    float3 col = pc.color0.rgb;
    col = lerp(col * 0.72, col * 1.55, clouds);

    // Faint accent wisps ride the brightest folds; a slow rising drift keeps them alive.
    const float rise = fbm(float2(p.x * 3.0, p.y * 4.0 + time * 0.12));
    const float wisp = pow(saturate(clouds), 3.0) * saturate(rise - 0.35) * 0.6;
    col += pc.color1.rgb * wisp;

    // Soft vignette toward the edges.
    const float2 c = uv - 0.5;
    const float vig = smoothstep(0.95, 0.32, length(c * float2(aspect, 1.0)));
    col *= lerp(0.55, 1.0, vig);

    col = lerp(pc.color0.rgb, col, intensity);
    return float4(col, 1.0);
}
