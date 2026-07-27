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

static const uint kShapeSdfGlyph = 4;

float hash11(float x)
{
    return frac(sin(x * 12.9898) * 43758.5453);
}

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
    UiClipDiscard(input);

    // Non-text shapes wearing this material: just pass their colour through untouched.
    if (input.type != kShapeSdfGlyph)
    {
        return input.color;
    }

    const float time = pc.params.x;

    uint texW, texH;
    g_textures[input.textureSlot].GetDimensions(texW, texH);
    const float2 texel = 1.0 / float2(max(texW, 1u), max(texH, 1u));
    const float row = input.position.y; // screen pixel row for scanlines / bands

    // A time-driven envelope that mostly sits low and occasionally spikes -> glitch bursts.
    const float burst = pow(saturate(sin(time * 3.0) * 0.5 + 0.5), 8.0)
                      + pow(saturate(sin(time * 11.0 + 1.7) * 0.5 + 0.5), 12.0);

    // Per-row jitter; a few rows jump hard, more often during a burst. Kept within the glyph's SDF
    // padding so the offset sampling never bleeds neighbouring atlas glyphs.
    const float rny = hash11(floor(row * 0.5) + floor(time * 18.0));
    const float jump = (rny > 0.93 ? 1.0 : 0.0) * (0.35 + burst);
    const float shift = (rny - 0.5) * texel.x * (1.5 + 9.0 * jump);

    // Chromatic split - sample coverage slightly left/right for R and B fringing.
    const float split = texel.x * (1.1 + 6.0 * burst);
    const float covR = SdfCov(input.textureSlot, input.texUV + float2(shift + split, 0.0));
    const float covG = SdfCov(input.textureSlot, input.texUV + float2(shift, 0.0));
    const float covB = SdfCov(input.textureSlot, input.texUV + float2(shift - split, 0.0));
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
