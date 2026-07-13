// Debug texture-preview pass: sample a source texture by its bindless slot and
// apply channel isolation / exposure / optional tonemap, writing an RGBA8 image
// the editor's Textures panel then displays. Fullscreen triangle, no per-draw
// descriptor set (bindless sampled images at set 0), mirroring tonemap.slang.

[[vk::binding(0, 0)]]
Texture2D g_textures[];

#include "include/FullScreenTriangle.slangh"

// Channel isolation modes.
static const uint kChannelRGBA = 0u;
static const uint kChannelR = 1u;
static const uint kChannelG = 2u;
static const uint kChannelB = 3u;
static const uint kChannelA = 4u;

// pc.flags bits.
static const uint kFlagTonemap = 1u; // apply HDR tonemap (float sources)

struct PreviewPush
{
    uint  srcSlot;     // bindless sampled slot of the source texture
    uint  channel;     // kChannel*
    float exposure;    // linear multiplier applied before channel/tonemap
    uint  flags;       // kFlag*
    uint  tonemapMode; // tonemap operator index (when kFlagTonemap set)
    uint  width;       // preview target width  (== source width, clamped)
    uint  height;      // preview target height
    uint  _pad;
};
[[vk::push_constant]] PreviewPush pc;

#include "include/Tonemap/TonemapOperators.slangh"

[shader("fragment")]
float4 fragmentMain(float4 svPos : SV_Position) : SV_Target0
{
    const int2  coord = int2(svPos.xy);
    const float4 texel = g_textures[pc.srcSlot].Load(int3(coord, 0));

    float3 rgb = texel.rgb * pc.exposure;
    if ((pc.flags & kFlagTonemap) != 0u)
    {
        rgb = TonemapByIndex(pc.tonemapMode, rgb);
    }

    // Isolated channels display as luminance (grayscale); RGBA keeps the image.
    float3 outColor;
    float  outAlpha = 1.0f;
    switch (pc.channel)
    {
        case kChannelR:
            outColor = float3(rgb.r, rgb.r, rgb.r);
            break;
        case kChannelG:
            outColor = float3(rgb.g, rgb.g, rgb.g);
            break;
        case kChannelB:
            outColor = float3(rgb.b, rgb.b, rgb.b);
            break;
        case kChannelA:
            outColor = float3(texel.a, texel.a, texel.a);
            break;
        default: // kChannelRGBA: keep colour + real alpha so the checkerboard shows through
            outColor = rgb;
            outAlpha = texel.a;
            break;
    }
    return float4(outColor, outAlpha);
}
