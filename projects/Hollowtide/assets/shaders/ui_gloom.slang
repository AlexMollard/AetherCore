// Hollowtide's atmosphere, as a UI effect shader ("ui_gloom"). Two instances of this one
// shader do all of it, told apart by params.z:
//
//   z = 0  GLOOM   background = true, drawn BEHIND the interface and over the parish.
//                  Mostly transparent: a vignette that closes in and a low fog that
//                  thickens, both driven by dread. The world has to stay visible through
//                  it, so the alpha it writes is the effect - never a full-screen wash.
//   z = 1  FLASH   an overlay on top of everything, for the instant something arrives.
//                  Zero alpha when params.w is zero, so it costs nothing while nothing is
//                  happening.
//
//   params = (time, dread 0..1, mode, surge | flash)
//   color0 = void base / flash tint,  color1 = dread deep

[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

struct BgPush
{
    float4 screenSize;
    float4 rect;
    float4 params;
    float4 color0;
    float4 color1;
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
    return lerp(lerp(hash21(i), hash21(i + float2(1, 0)), u.x),
                lerp(hash21(i + float2(0, 1)), hash21(i + float2(1, 1)), u.x), u.y);
}

float fbm(float2 p)
{
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 4; ++i) { s += a * vnoise(p); p *= 2.03; a *= 0.5; }
    return s;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    const float time  = pc.params.x;
    const float dread = saturate(pc.params.y);
    const float mode  = pc.params.z;
    const float amt   = saturate(pc.params.w);

    const float2 uv = input.uv;
    const float2 centred = (uv - 0.5) * float2(1.0, pc.rect.w / max(pc.rect.z, 1.0));

    // ── FLASH ────────────────────────────────────────────────────────────────────────
    // Something came into the room. A hard wash that drops off fast, torn across a few
    // horizontal bands so it reads as a failure of the picture rather than a fade.
    if (mode > 0.5)
    {
        if (amt <= 0.002) { discard; }

        const float band = step(0.55, vnoise(float2(time * 9.0, uv.y * 40.0)));
        const float tear = band * amt * 0.45;
        const float grain = hash21(uv * pc.screenSize.xy + time) * 0.35;

        // Brightest at the edges and thin in the middle, so the reading matter stays
        // readable even at full strength - a horror game that hides its own numbers on the
        // frame something happens is just an interruption.
        const float edge = saturate(length(centred) * 1.7);
        const float alpha = amt * (0.28 + edge * 0.55 + tear);
        float3 col = lerp(pc.color1.rgb, pc.color0.rgb, 0.35 + grain * 0.4 + tear);
        return float4(col, saturate(alpha));
    }

    // ── GLOOM ────────────────────────────────────────────────────────────────────────
    // A vignette that closes in with dread, plus fog crawling up from the bottom. Both are
    // written as ALPHA over the parish, never as a colour that replaces it.
    const float radial = length(centred * float2(1.15, 1.35));
    const float vignette = smoothstep(0.34, 1.02, radial);

    // The fog moves at two speeds in opposite directions - one field alone reads as a
    // scrolling texture, two crossing read as something turning over.
    const float2 drift = float2(time * 0.014, -time * 0.021);
    const float fog = fbm(uv * float2(3.1, 2.2) + drift)
                    * fbm(uv * float2(1.7, 1.1) - drift * 1.6);
    const float lowFog = smoothstep(0.15, 1.0, uv.y) * fog;

    // At rest this is a faint frame around the picture. At full dread it is most of the
    // screen, and the parish is only visible where its own lanterns are lighting it.
    const float creep = 0.16 + dread * 0.62;
    float alpha = saturate(vignette * (0.42 + dread * 0.50) + lowFog * creep);

    // The last quarter of the meter breathes: an unsteady alpha, slow enough to feel and
    // not fast enough to notice, which is what makes a still screen uncomfortable.
    const float unease = smoothstep(0.7, 1.0, dread);
    alpha *= 1.0 + sin(time * 1.7) * 0.06 * unease + sin(time * 0.41) * 0.04;

    float3 col = lerp(pc.color0.rgb, pc.color1.rgb, dread * (0.35 + fog * 0.5));
    // A little grain so a flat dark area does not band on an 8-bit display.
    col += (hash21(uv * pc.screenSize.xy * 0.5) - 0.5) * 0.012;

    return float4(col, saturate(alpha));
}
