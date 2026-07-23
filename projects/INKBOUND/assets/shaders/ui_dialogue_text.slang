// INKBOUND dialogue body material. Per-glyph rich-text effects selected by params.y (InkEffect id).
// Masked to real glyph SDF geometry via the shared ui_shapes vertex (same contract as ui_glitch_text).
// params.x = time, params.y = effectId, params.z = strength; color0 = base (0 => glyph colour),
// color1 = accent. Offsets stay within the glyph SDF padding so sampling never bleeds neighbours.
//
// effectId: 0 normal, 1 shake, 2 wave, 3 flicker, 4 whisper, 5 glitch (see DialogueMarkup.InkEffect).

// Bindless set 0 - MUST match ui_shapes.slang so the shared vertex + this fragment agree.
[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

struct ShapesPush
{
    float4   screenSize;
    uint64_t commandData; // unused here; present for layout parity with the vertex
    uint     pad0;
    uint     pad1;
    float4   params;      // x=time, y=effectId, z=strength
    float4   color0;      // base colour (0,0,0 -> use the glyph's own colour)
    float4   color1;      // accent colour
};
[[vk::push_constant]] ShapesPush pc;

// Must match ui_shapes.slang VSOutput exactly (locations + interpolation).
struct VSOutput
{
    float4 position      : SV_Position;
    float4 color         : TEXCOORD0;
    float2 local         : TEXCOORD1;
    float2 localScale    : TEXCOORD2;
    float  cornerRadius  : TEXCOORD3;
    nointerpolation uint type        : TEXCOORD4;
    float2 texUV         : TEXCOORD5;
    nointerpolation uint textureSlot : TEXCOORD6;
    nointerpolation uint flags       : TEXCOORD7;
};

static const uint kShapeSdfGlyph = 4;

float hash11(float x) { return frac(sin(x * 12.9898) * 43758.5453); }

// Antialiased SDF glyph coverage at an atlas uv (single-channel R8 SDF).
float SdfCov(uint slot, float2 uv)
{
    const float d = g_textures[slot].Sample(g_linearSampler, uv).r;
    const float w = clamp(fwidth(d) * 0.7, 0.001, 0.5);
    return smoothstep(0.5 - w, 0.5 + w, d);
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    // Non-text shapes wearing this material: pass their colour through untouched.
    if (input.type != kShapeSdfGlyph)
    {
        return input.color;
    }

    const float time = pc.params.x;
    const uint  fx   = (uint)(pc.params.y + 0.5);
    const float amp  = pc.params.z <= 0.0 ? 1.0 : pc.params.z;

    uint texW, texH;
    g_textures[input.textureSlot].GetDimensions(texW, texH);
    const float2 texel = 1.0 / float2(max(texW, 1u), max(texH, 1u));

    // Per-glyph phase from screen position so motion differs glyph-to-glyph (blocky, not rigid).
    const float gphase = input.position.x * 0.15 + input.position.y * 0.07;

    float2 off = float2(0.0, 0.0);
    float  bright = 1.0;
    float  alphaMul = 1.0;
    float3 tint = float3(1.0, 1.0, 1.0);

    if (fx == 1) // shake
    {
        const float jx = hash11(floor(time * 30.0) + gphase) - 0.5;
        const float jy = hash11(floor(time * 30.0) + gphase + 7.0) - 0.5;
        off = float2(jx, jy) * texel * (2.5 * amp);
    }
    else if (fx == 2) // wave
    {
        off = float2(0.0, sin(time * 4.0 + gphase * 2.0) * texel.y * (3.0 * amp));
    }
    else if (fx == 3) // flicker
    {
        const float f = hash11(floor(time * 22.0) + gphase);
        bright = f > 0.88 ? 0.25 : (0.85 + 0.15 * f);
        alphaMul = f > 0.94 ? 0.4 : 1.0;
    }
    else if (fx == 4) // whisper: dim, slow breath, faint chroma bleed
    {
        bright = 0.6 + 0.12 * sin(time * 1.6 + gphase);
        off = float2(sin(time * 0.8 + gphase) * texel.x * 0.6 * amp, 0.0);
        tint = lerp(float3(1.0, 1.0, 1.0), pc.color1.rgb, 0.15);
    }
    else if (fx == 5) // glitch: chromatic split + row jump
    {
        const float rny = hash11(floor(input.position.y * 0.5) + floor(time * 18.0));
        const float jump = rny > 0.93 ? 1.0 : 0.0;
        off = float2((rny - 0.5) * texel.x * (1.5 + 9.0 * jump), 0.0);
    }

    // Per-glyph ink birth: params.w = 1 settled, 0 freshly revealed. A fresh glyph reads as a fat, wet
    // ink blob - the SDF iso is lowered (dilated) with a noisy irregular edge and tinted deep teal - then
    // it dries into the crisp letter as birth -> 1.
    const float birth = saturate(pc.params.w);
    const float wet   = 1.0 - birth;

    const float2 sampUV = input.texUV + off;
    const float  d = g_textures[input.textureSlot].Sample(g_linearSampler, sampUV).r;

    // Irregular blob edge: perturb the iso threshold with position/time value-noise while wet.
    const float en  = hash11(floor(input.position.x * 0.7) + floor(input.position.y * 0.7) * 91.0
                             + floor(time * 12.0)) - 0.5;
    const float iso = 0.5 - wet * 0.16 - wet * 0.08 * en; // fatter + rougher when wet
    const float aa  = clamp(fwidth(d) * 0.7, 0.001, 0.5) + wet * 0.09; // gooier edge when wet
    const float cov = smoothstep(iso - aa, iso + aa, d);
    if (cov <= 0.003)
    {
        discard;
    }

    float3 base  = dot(pc.color0.rgb, float3(1.0, 1.0, 1.0)) > 0.001 ? pc.color0.rgb : input.color.rgb;
    float3 crisp = base * tint * bright;

    // Wet ink is a dark teal; sharpen toward the glyph colour as it dries, with a brief accent sheen
    // that peaks mid-birth so the letter "sets" with a wet highlight.
    const float3 inkWet = float3(0.05, 0.20, 0.26);
    float3 rgb = lerp(crisp, inkWet, wet * 0.85);
    rgb += pc.color1.rgb * wet * (1.0 - wet) * 0.6;

    if (fx == 5) // glitch: add an accent fringe on the split
    {
        const float split = texel.x * 1.6;
        const float cr = SdfCov(input.textureSlot, input.texUV + off + float2(split, 0.0));
        rgb += pc.color1.rgb * max(cr - cov, 0.0) * 1.1;
    }

    const float birthAlpha = smoothstep(0.0, 0.15, birth); // quick fade-in so the blob never hard-pops
    return float4(rgb, input.color.a * cov * alphaMul * birthAlpha);
}
