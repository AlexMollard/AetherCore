// Glitch-ink UI material fragment (UIMaterial component, "ui_glitch_text").
// Pairs with the engine's shared ui_shapes VERTEX shader, so it receives the real text-glyph
// geometry + font atlas and the effect is masked to the actual letter shapes (not a bg quad).
// It samples the SDF atlas with chromatic-split offsets, adds scanlines, occasional row jumps and
// an accent fringe for a broken-signal inky look. params.x = time; color0 = ink/base (0 = use the
// element's own colour); color1 = accent fringe/glitch colour.

// Bindless set 0 - MUST match ui_shapes.slang so the shared vertex + this fragment agree.
[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

// Shared with the engine's UI vertex stage. Do not hand-copy VSOutput - include it.
#include "include/UiVertexOut.slangh"

// Push layout MUST match the C++ MaterialPush / the ui_shapes ShapesPush prefix. The first 32 bytes
// (screenSize + commandData + pads) are what the shared vertex reads; the fx fields are ours.
struct ShapesPush
{
    float4   screenSize;
    uint64_t commandData; // unused here; present for layout parity with the vertex
    uint     pad0;
    uint     pad1;
    float4   params;      // x = time
    float4   color0;      // ink / base colour (0,0,0 -> use the glyph's own colour)
    float4   color1;      // accent fringe / glitch colour
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

float hash11(float x)
{
    return frac(sin(x * 12.9898) * 43758.5453);
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    UiClipDiscard(input);

    // Non-text shapes wearing this material: just pass their colour through untouched.
    if (input.type != kShapeGlyph)
    {
        return input.color;
    }

    const float time = pc.params.x;

    const float2 texel = GlyphPixel(input); // one screen pixel, in glyph space
    const float row = input.position.y;     // screen pixel row for scanlines / bands

    // A time-driven envelope that mostly sits low and occasionally spikes -> glitch bursts.
    const float burst = pow(saturate(sin(time * 3.0) * 0.5 + 0.5), 8.0)
                      + pow(saturate(sin(time * 11.0 + 1.7) * 0.5 + 0.5), 12.0);

    // Per-row jitter; a few rows jump hard, more often during a burst. Offsets are free to be
    // any size now - an outline has no neighbour in an atlas to bleed into.
    const float rny = hash11(floor(row * 0.5) + floor(time * 18.0));
    const float jump = (rny > 0.93 ? 1.0 : 0.0) * (0.35 + burst);
    const float shift = (rny - 0.5) * texel.x * (1.5 + 9.0 * jump);

    // Chromatic split - sample coverage slightly left/right for R and B fringing.
    const float split = texel.x * (1.1 + 6.0 * burst);
    const float covR = GlyphCov(input, float2(shift + split, 0.0));
    const float covG = GlyphCov(input, float2(shift, 0.0));
    const float covB = GlyphCov(input, float2(shift - split, 0.0));
    const float cov = max(covG, max(covR, covB));
    if (cov <= 0.003)
    {
        discard;
    }

    // Base ink colour: color0 if authored, else the glyph's own colour.
    const float3 base = dot(pc.color0.rgb, float3(1.0, 1.0, 1.0)) > 0.001 ? pc.color0.rgb : input.color.rgb;

    float3 rgb = base * covG;
    // Accent fringe rides the split difference (cyan on one edge, warm on the other).
    rgb += pc.color1.rgb * max(covR - covG, 0.0) * 1.1;
    rgb += float3(0.9, 0.2, 0.35) * max(covB - covG, 0.0) * 0.6;

    // Rolling scanlines + a faint constant darkening between them.
    const float scan = 0.82 + 0.18 * sin(row * 3.14159 + time * 6.0);
    rgb *= scan;

    // Occasional bright glitch band flashes the accent colour.
    rgb += pc.color1.rgb * jump * 0.6;

    const float alpha = input.color.a * cov;
    return float4(rgb, alpha);
}
