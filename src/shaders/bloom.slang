// Bloom: progressive downsample, then upsample accumulating back up the chain.
//
// No brightness threshold. A threshold decides per pixel whether light "counts",
// so a highlight drifting across it pops in and out between frames and along an
// edge - the classic shimmering bloom. Instead every pixel contributes, weighted
// by how bright it is relative to the rest, and the tonemapper decides what reads
// as glowing. That is also closer to what a lens actually does: it scatters all
// the light landing on it, not only the parts above some value.
//
// The filters are Jimenez's (SIGGRAPH 2014 "Next Generation Post Processing in
// Call of Duty: Advanced Warfare"): a 13-tap downsample whose taps sit so that no
// frequency aliases into the next mip, and a 3x3 tent on the way back up so the
// chain reconstructs smoothly instead of showing box-filter blocks.

[[vk::binding(0, 0)]] Texture2D    g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

#include "include/FullScreenTriangle.slangh"

struct BloomPush
{
    uint  srcSlot;
    float srcWidth;
    float srcHeight;
    float filterRadius;  // upsample only, in source texels
    float karisAverage;  // >0.5 on the first downsample only
};
[[vk::push_constant]] BloomPush pc;

// Luminance-weighted average of the first downsample's taps. Fireflies - single
// pixels far brighter than their neighbours - otherwise survive the whole chain
// and flicker as a bright dot wherever they land. Weighting each tap by 1/(1+luma)
// lets a wide, genuinely bright area still bloom while a lone spike cannot.
float3 KarisAverage(float3 a, float3 b, float3 c, float3 d)
{
    const float3 luma = float3(0.2126, 0.7152, 0.0722);
    float wa = 1.0 / (1.0 + dot(a, luma));
    float wb = 1.0 / (1.0 + dot(b, luma));
    float wc = 1.0 / (1.0 + dot(c, luma));
    float wd = 1.0 / (1.0 + dot(d, luma));
    return (a * wa + b * wb + c * wc + d * wd) / max(wa + wb + wc + wd, 1e-5);
}

// The global sampler addresses REPEAT, so a tap reaching past an edge wraps to the
// opposite side of the screen and drags whatever is bright there back in as a rim
// along the border. Clamping to the half-texel inside the edge keeps every tap on
// the image it belongs to.
float3 Sample(float2 uv)
{
    const float2 halfTexel = float2(0.5 / pc.srcWidth, 0.5 / pc.srcHeight);
    const float2 clamped = clamp(uv, halfTexel, 1.0 - halfTexel);
    return g_textures[pc.srcSlot].SampleLevel(g_linearSampler, clamped, 0.0).rgb;
}

[shader("fragment")]
float4 downsampleMain(float4 svPos : SV_Position) : SV_Target0
{
    const float2 texel = float2(1.0 / pc.srcWidth, 1.0 / pc.srcHeight);
    const float2 uv = svPos.xy * texel * 2.0; // destination is half the source

    const float x = texel.x;
    const float y = texel.y;

    // 13 taps: an inner quad of four, an outer ring of four corners, plus the
    // centre cross. The weights below sum to 1.
    float3 a = Sample(uv + float2(-2.0 * x,  2.0 * y));
    float3 b = Sample(uv + float2( 0.0,      2.0 * y));
    float3 c = Sample(uv + float2( 2.0 * x,  2.0 * y));
    float3 d = Sample(uv + float2(-2.0 * x,  0.0));
    float3 e = Sample(uv);
    float3 f = Sample(uv + float2( 2.0 * x,  0.0));
    float3 g = Sample(uv + float2(-2.0 * x, -2.0 * y));
    float3 h = Sample(uv + float2( 0.0,     -2.0 * y));
    float3 i = Sample(uv + float2( 2.0 * x, -2.0 * y));
    float3 j = Sample(uv + float2(-x,  y));
    float3 k = Sample(uv + float2( x,  y));
    float3 l = Sample(uv + float2(-x, -y));
    float3 m = Sample(uv + float2( x, -y));

    float3 result;
    if (pc.karisAverage > 0.5)
    {
        // Each group is one box of four taps; averaging within a group before
        // weighting is what makes the firefly suppression stable.
        result  = KarisAverage(j, k, l, m) * 0.5;
        result += KarisAverage(a, b, d, e) * 0.125;
        result += KarisAverage(b, c, e, f) * 0.125;
        result += KarisAverage(d, e, g, h) * 0.125;
        result += KarisAverage(e, f, h, i) * 0.125;
    }
    else
    {
        result  = e * 0.125;
        result += (a + c + g + i) * 0.03125;
        result += (b + d + f + h) * 0.0625;
        result += (j + k + l + m) * 0.125;
    }

    return float4(max(result, 0.0), 1.0);
}

[shader("fragment")]
float4 upsampleMain(float4 svPos : SV_Position) : SV_Target0
{
    // Destination is twice the source, so a destination pixel maps to half a
    // source texel. The radius is in source texels to keep the blur width
    // consistent as the chain widens.
    const float2 texel = float2(1.0 / pc.srcWidth, 1.0 / pc.srcHeight);
    const float2 uv = (svPos.xy * 0.5) * texel;

    const float x = texel.x * pc.filterRadius;
    const float y = texel.y * pc.filterRadius;

    float3 a = Sample(uv + float2(-x,  y));
    float3 b = Sample(uv + float2(0.0, y));
    float3 c = Sample(uv + float2( x,  y));
    float3 d = Sample(uv + float2(-x, 0.0));
    float3 e = Sample(uv);
    float3 f = Sample(uv + float2( x, 0.0));
    float3 g = Sample(uv + float2(-x, -y));
    float3 h = Sample(uv + float2(0.0, -y));
    float3 i = Sample(uv + float2( x, -y));

    // 3x3 tent, weights 1-2-1 in each axis over 16.
    float3 result  = e * 4.0;
    result += (b + d + f + h) * 2.0;
    result += (a + c + g + i);
    result /= 16.0;

    return float4(max(result, 0.0), 1.0);
}
