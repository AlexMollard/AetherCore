// INKBOUND dialogue body material. Per-glyph rich-text effects selected by params.y (InkEffect id).
// Masked to real glyph SDF geometry via the shared ui_shapes vertex (same contract as ui_glitch_text).
// params.x = time, params.y = effectId, params.z = strength; color0 = base (0 => glyph colour),
// color1 = accent. Offsets stay within the glyph SDF padding so sampling never bleeds neighbours.
//
// effectId: 0 normal, 1 shake, 2 wave, 3 flicker, 4 whisper, 5 glitch (see DialogueMarkup.InkEffect).

// Bindless set 0 - MUST match ui_shapes.slang so the shared vertex + this fragment agree.
[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

// Shared with the engine's UI vertex stage. Do not hand-copy VSOutput - include it.
#include "include/UiVertexOut.slangh"

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


#include "text_curves.slangh"

static const uint kShapeGlyph = 4;

// Coverage of THIS glyph at the fragment's own position, displaced by `offset` in glyph-local
// space (1.0 = the glyph's whole box). This replaces sampling an SDF atlas: there is no atlas
// any more, so the outline is evaluated directly and the old texel-sized offsets become
// screen-pixel-sized ones, which is what these effects always meant anyway.
float GlyphCov(VSOutput input, float2 offset)
{
    Texture2D curves = g_textures[NonUniformResourceIndex(input.textureSlot)];
    uint w, h;
    curves.GetDimensions(w, h);
    const float2 local = float2(input.local.x * 0.5 + 0.5, 0.5 - input.local.y * 0.5) + offset;
    const float2 px = 1.0 / max(input.localScale, float2(1e-4, 1e-4));
    return GlyphCoverage(curves, w, local, px,
        uint(input.glyphBands.x), uint(input.glyphBands.y), uint(input.glyphBands.z));
}

// One screen pixel, measured in this glyph's local space.
float2 GlyphPixel(VSOutput input)
{
    return 1.0 / max(input.localScale, float2(1e-4, 1e-4));
}


float hash11(float x) { return frac(sin(x * 12.9898) * 43758.5453); }


[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    UiClipDiscard(input);

    // Non-text shapes wearing this material: pass their colour through untouched.
    if (input.type != kShapeGlyph)
    {
        return input.color;
    }

    const float time = pc.params.x;
    const uint  fx   = (uint)(pc.params.y + 0.5);
    const float amp  = pc.params.z <= 0.0 ? 1.0 : pc.params.z;

    // One screen pixel in glyph space. The effects below were written against atlas texels;
    // pixels are what they always meant, and an outline has no texels to measure in.
    const float2 texel = GlyphPixel(input);

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

    // Irregular blob edge: the old path fattened the letter by lowering the SDF iso, which an
    // outline has no equivalent of. Dilating the COVERAGE instead gives the same wet blob - take
    // the widest of a few offset evaluations, jittered by the same value-noise - and collapses
    // to a single evaluation the moment the glyph has dried.
    const float en  = hash11(floor(input.position.x * 0.7) + floor(input.position.y * 0.7) * 91.0
                             + floor(time * 12.0)) - 0.5;
    float cov = GlyphCov(input, off);
    if (wet > 0.001)
    {
        const float2 grow = texel * (1.5 + 0.9 * en) * wet;
        cov = max(cov, GlyphCov(input, off + float2(grow.x, 0.0)));
        cov = max(cov, GlyphCov(input, off - float2(grow.x, 0.0)));
        cov = max(cov, GlyphCov(input, off + float2(0.0, grow.y)));
        cov = max(cov, GlyphCov(input, off - float2(0.0, grow.y)));
    }
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
        const float cr = GlyphCov(input, off + float2(split, 0.0));
        rgb += pc.color1.rgb * max(cr - cov, 0.0) * 1.1;
    }

    const float birthAlpha = smoothstep(0.0, 0.15, birth); // quick fade-in so the blob never hard-pops
    return float4(rgb, input.color.a * cov * alphaMul * birthAlpha);
}
