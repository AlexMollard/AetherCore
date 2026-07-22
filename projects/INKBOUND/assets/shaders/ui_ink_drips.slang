// Ink drips running down from under the title (UIEffect, "ui_ink_drips", overlay).
// A fullscreen quad; the drips are placed in SCREEN space from the wordmark's live rect so they
// stay under the letters at any canvas scale. Each drip is a hanging thread that swells into a
// teardrop bulb, drops a falling bead, then dries - not a static rectangle. Pixel-art snapped.
//   params = (time, originY_px, xMin_px, xMax_px);  color0 = ink body;  color1 = accent rim.

[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

struct DripPush
{
    float4 screenSize; // .xy = viewport px
    float4 rect;       // x, y, w, h in px (fullscreen)
    float4 params;     // x=time, y=originY px, z=xMin px, w=xMax px
    float4 color0;     // ink body
    float4 color1;     // accent rim
};

[[vk::push_constant]] DripPush pc;

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

float hash11(float x) { return frac(sin(x * 91.345) * 47453.11); }
float easeIn(float x) { return x * x; }

// One ink drip in screen px. `ox` is its column, `oy` the origin row (wordmark bottom). Returns
// coverage (0..1). It hangs, swells into a teardrop, and sheds a falling bead late in the cycle.
float dripMask(float2 p, float ox, float oy, float phase, float time, float period, float maxDrop)
{
    const float lt = frac((time + phase) / period);
    const float dx = p.x - ox;

    // Leading head travels down; the hanging thread runs oy..headY and narrows toward the head.
    const float headY = oy + maxDrop * easeIn(smoothstep(0.0, 0.9, lt));
    const float below = p.y - oy;
    const float span = max(headY - oy, 1.0);
    const float taper = lerp(1.0, 0.35, saturate(below / span));
    const float threadW = 3.5 * taper;
    const float thread = (p.y >= oy && p.y <= headY && abs(dx) <= threadW) ? 1.0 : 0.0;

    // Teardrop bulb at the head (swells as it descends into a fat drop before it sheds).
    const float bulbR = lerp(4.0, 8.5, saturate(lt));
    const float bulb = length(float2(dx, p.y - headY)) <= bulbR ? 1.0 : 0.0;

    // A round bead detaches near the end of the cycle and accelerates down past the head.
    const float dropT = saturate((lt - 0.6) / 0.4);
    const float beadY = headY + maxDrop * 0.9 * easeIn(dropT);
    const float beadR = lerp(6.0, 2.5, dropT);
    const float bead = (dropT > 0.0 && length(float2(dx, p.y - beadY)) <= beadR) ? 1.0 : 0.0;

    return max(thread, max(bulb, bead));
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    const float time = pc.params.x;
    const float oy   = pc.params.y;
    const float xMin = pc.params.z;
    const float xMax = pc.params.w;
    if (xMax <= xMin) // wordmark rect not supplied yet
    {
        discard;
    }

    // Pixel-art: snap screen position to a coarse grid so the drips read as chunky ink.
    const float2 rawP = input.uv * pc.screenSize.xy;
    const float pix = 5.0;
    const float2 p = floor(rawP / pix) * pix + pix * 0.5;

    // Four drips under the wordmark. The rect box is much wider than the left-aligned "INKBOUND"
    // text, so keep the columns in the left ~half of the box where the actual letters sit.
    const float span = xMax - xMin;
    const float xs[4] = { 0.03, 0.12, 0.20, 0.28 };
    const float phase[4] = { 0.0, 2.3, 1.1, 3.4 };
    const float period[4] = { 3.6, 4.4, 4.0, 5.0 };
    const float maxDrop[4] = { 46.0, 60.0, 38.0, 54.0 };

    float ink = 0.0;
    float dry = 0.0; // cycle progress of whichever drip covers this pixel (for the wet->dry fade)
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        const float ox = xMin + span * xs[i];
        const float m = dripMask(p, ox, oy, phase[i], time, period[i], maxDrop[i]);
        if (m > ink)
        {
            ink = m;
            dry = frac((time + phase[i]) / period[i]);
        }
    }
    if (ink < 0.5)
    {
        discard;
    }

    // Wet at the top of the cycle, drying (fading) toward the end.
    const float alpha = 0.85 * (1.0 - smoothstep(0.55, 1.0, dry));

    // Ink body with an accent rim near the leading edge (approximated by proximity to the origin row).
    const float rim = 1.0 - smoothstep(0.0, 28.0, p.y - oy);
    float3 rgb = lerp(pc.color0.rgb, pc.color1.rgb, saturate(rim) * 0.7);
    return float4(rgb, alpha);
}
