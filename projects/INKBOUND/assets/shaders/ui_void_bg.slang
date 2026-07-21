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

// Pixel-art helpers. kPixelsWide sets the virtual horizontal resolution the whole backdrop is
// snapped to, so it reads as chunky authored pixels regardless of window size. Posterize then
// bands the colour to a small palette for the flat, limited-colour look.
static const float kPixelsWide = 300.0;

float2 Pixelate(float2 uv, float aspect)
{
    const float2 res = float2(kPixelsWide, max(kPixelsWide / max(aspect, 0.001), 1.0));
    return (floor(uv * res) + 0.5) / res;
}

// A watching eye in the void, in aspect-corrected space. Hard-edged almond lens (so it stays
// chunky on the pixel grid) with a dark pupil slit; `open` 0..1 scales the eyelid. Returns a
// small palette: ~1 in the glowing core, a dim rim around it, 0 elsewhere.
float EyeShape(float2 p, float2 c, float w, float open)
{
    if (open <= 0.001)
    {
        return 0.0;
    }
    const float2 d = p - c;
    const float xn = d.x / w;
    if (abs(xn) >= 1.0)
    {
        return 0.0;
    }
    const float halfH = (1.0 - xn * xn) * w * 0.62 * open; // almond half-height at this x
    const float core = step(abs(d.y), halfH);
    const float rim = step(abs(d.y), halfH + w * 0.12);
    // Vertical slit pupil - a reptilian, watching look.
    const float pupil = step(abs(d.x), w * 0.22) * step(abs(d.y), halfH * 0.85);
    return saturate(max(core - pupil, rim * 0.35));
}

// Mostly-shut eye that blinks open briefly once per period.
float EyeBlink(float t, float phase, float period)
{
    const float u = frac((t + phase) / period);
    return u > 0.80 ? sin((u - 0.80) / 0.20 * 3.14159265) : 0.0;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    const float time = pc.params.x;
    const float intensity = pc.params.y > 0.0 ? pc.params.y : 1.0;

    // Aspect-correct domain so clouds aren't stretched.
    const float aspect = pc.screenSize.x / max(pc.screenSize.y, 1.0);
    // Snap to a coarse virtual-pixel grid so the whole backdrop reads as chunky pixel art.
    const float2 uv = Pixelate(input.uv, aspect);
    const float2 p = float2(uv.x * aspect, uv.y);

    // Two layers of slow roiling ink; a small domain-warp gives it a liquid, folded look.
    const float2 warp = float2(fbm(p * 2.0 + time * 0.02), fbm(p * 2.0 - time * 0.015)) - 0.5;
    const float n1 = fbm(p * 2.6 + warp * 0.8 + float2(time * 0.025, time * 0.015));
    const float n2 = fbm(p * 5.5 - float2(time * 0.02, 0.0));
    const float clouds = smoothstep(0.30, 0.78, n1 * 0.72 + n2 * 0.28);

    // A slow rising drift feeds the faint accent crests.
    const float rise = fbm(float2(p.x * 3.0, p.y * 4.0 + time * 0.12));
    const float wisp = pow(saturate(clouds), 3.0) * saturate(rise - 0.35);

    // Flat pixel-art shading: fold cloud + wisp into a single tone, quantise it into a few steps,
    // then map through a small hand-built ramp. Colouring from a ramp (not RGB posterise) keeps the
    // palette dark and ink-blue - posterising near-black RGB just greys and brightens it.
    float tone = saturate(clouds * 0.85 + wisp * 0.6);
    const float bands = 5.0;
    tone = floor(tone * (bands - 1.0) + 0.5) / (bands - 1.0); // nearest of `bands` flat steps, 0..1

    const float3 deep = pc.color0.rgb * 0.40;                       // trough shadow
    const float3 base = pc.color0.rgb * 1.15;                       // mid void
    const float3 lift = lerp(pc.color0.rgb * 1.7, pc.color1.rgb, 0.16); // faint cyan crest
    float3 col = tone < 0.5 ? lerp(deep, base, tone * 2.0) : lerp(base, lift, (tone - 0.5) * 2.0);

    // Watching eyes peer from the void - scattered across the right + edges (clear of the menu
    // text), each on its own staggered blink, glowing in the pale accent colour.
    float eyeI = 0.0;
    eyeI = max(eyeI, EyeShape(p, float2(0.72 * aspect, 0.30), 0.015 * aspect, EyeBlink(time, 0.0, 9.0)));
    eyeI = max(eyeI, EyeShape(p, float2(0.88 * aspect, 0.54), 0.013 * aspect, EyeBlink(time, 3.3, 11.0)));
    eyeI = max(eyeI, EyeShape(p, float2(0.62 * aspect, 0.73), 0.016 * aspect, EyeBlink(time, 6.7, 13.0)));
    eyeI = max(eyeI, EyeShape(p, float2(0.94 * aspect, 0.20), 0.012 * aspect, EyeBlink(time, 9.1, 10.0)));
    eyeI = max(eyeI, EyeShape(p, float2(0.52 * aspect, 0.45), 0.014 * aspect, EyeBlink(time, 1.7, 12.0)));
    col = lerp(col, pc.color1.rgb, saturate(eyeI) * 0.85);

    // Vignette, banded too so it stays pixel-consistent.
    const float2 c = uv - 0.5;
    float vig = smoothstep(0.95, 0.30, length(c * float2(aspect, 1.0)));
    vig = floor(vig * 4.0 + 0.5) / 4.0;
    col *= lerp(0.5, 1.0, vig);

    col = lerp(pc.color0.rgb, col, intensity);
    return float4(col, 1.0);
}
