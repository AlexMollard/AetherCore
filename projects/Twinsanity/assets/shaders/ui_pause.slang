// The Twinsanity pause menu's and HUD's code-drawn shapes ("ui_pause", UIMaterial).
//
// The PS2 game draws the menu's discs, ball and gem track as coloured geometry (no texture for
// any of them exists on the disc), so they are analytic shapes here. The menu is a redesign
// rather than the original's swirl: the same discs, ball, gem track and option drum laid out
// in a grid straight over a vignette dim (TwinsanityPause.cs lays it out).
//
// params = (time, shape, a, b) with shape:
//   1 disc     - a counter disc: graded fill lit from the top left (color0 lit, color1 shaded),
//                white rim from r=0.90;
//   2 ball     - the badge's ball, same construction, rim from 0.93;
//   4 sparkle  - the HUD's gem-pickup sparkle (TwinsanityHud.LayoutGemPop): a bright soft blob in
//                color0 with a four-point glint and a white-hot core;
//   5 dim      - the full-screen backdrop: a navy dim, darker toward the edges (vignette);
//   6 box      - a rounded box with a soft drop shadow: a = corner radius px, b = shadow px (the
//                element's rect includes b px of shadow on every side). Vertical gradient
//                color0 (top) to color1 (bottom); color1.a = the white rim's strength;
//   7 glow box - shape 6 with a pulsing cyan outer glow instead of the shadow (the selection).
// The element's image alpha (input.color.a) fades the whole shape, so C# fades the menu in and out.

[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

#include "include/UiVertexOut.slangh"

struct PausePush
{
    float4   screenSize;
    uint64_t commandData;
    uint     pad0;
    uint     pad1;
    float4   params;
    float4   color0;
    float4   color1;
};

[[vk::push_constant]] PausePush pc;

static const float3 PALE      = float3(0.94, 0.97, 1.0);
static const float3 NAVY      = float3(0.008, 0.02, 0.055);
static const float3 GLOW_CYAN = float3(0.45, 0.95, 1.0);

// Signed distance to a rounded box of half-extent b and corner radius r, in pixels.
float RoundBox(float2 p, float2 b, float r)
{
    const float2 q = abs(p) - (b - r);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    UiClipDiscard(input);

    const int shape = int(pc.params.y + 0.5);
    float alpha = 0.0;
    float3 col = PALE;

    if (shape == 1 || shape == 2)
    {
        const float d = length(input.local);
        if (d > 1.0) { discard; }
        const float rim = shape == 2 ? 0.93 : 0.90;
        const float lit = saturate(0.5 - 0.5 * (input.local.x + input.local.y));
        // Anti-aliased rim edge: one pixel wide at any size.
        const float px = 2.0 / max(min(input.localScale.x, input.localScale.y), 1.0);
        col = lerp(lerp(pc.color1.rgb, pc.color0.rgb, lit), PALE, smoothstep(rim - px, rim, d));
        alpha = smoothstep(1.0, 1.0 - px, d);
    }
    else if (shape == 4)
    {
        const float2 q = abs(input.local);
        const float r = length(input.local);
        // A full soft blob in the gem's colour (the rig's ring reads as saturated glowing dots),
        // a faint glint, and a small white-hot centre.
        const float blob = saturate(1.0 - r);
        const float halo = blob * sqrt(blob);
        const float glint = blob * saturate(1.0 - min(q.x, q.y) * 10.0) * 0.5;
        const float core = exp(-r * r * 70.0);
        alpha = saturate(halo + glint + core);
        if (alpha <= 0.002) { discard; }
        col = lerp(saturate(pc.color0.rgb * 1.25), float3(1.0, 1.0, 1.0), saturate(core));
    }
    else if (shape == 5)
    {
        const float v = smoothstep(0.15, 1.45, length(input.local));
        col = NAVY;
        alpha = lerp(0.55, 0.86, v);
    }
    else
    {
        // Rounded box (6) / glowing selection box (7), in pixels about the rect centre.
        const float2 size = input.localScale;
        const float2 p = input.local * size * 0.5;
        const float margin = max(pc.params.w, 0.0);
        const float2 hb = max(size * 0.5 - margin, float2(1.0, 1.0));
        const float radius = min(pc.params.z, min(hb.x, hb.y));
        const float d = RoundBox(p, hb, radius);
        const float body = saturate(0.5 - d);

        // Fill: vertical gradient with a soft top sheen, and a white rim just inside the edge.
        const float t = saturate(p.y / hb.y * 0.5 + 0.5);
        float3 fill = lerp(pc.color0.rgb, pc.color1.rgb, t);
        fill += 0.08 * smoothstep(0.45, 0.0, t);
        const float rimW = clamp(radius * 0.1, 1.5, 3.5);
        const float rim = smoothstep(-rimW - 1.0, -rimW, d) * pc.color1.a;
        fill = lerp(fill, PALE, rim);

        float3 outer = NAVY;
        float outerA = 0.0;
        if (margin > 0.0)
        {
            if (shape == 7)
            {
                const float pulse = 0.8 + 0.2 * sin(pc.params.x * 6.0);
                const float g = saturate(1.0 - max(d, 0.0) / margin);
                outer = GLOW_CYAN;
                outerA = 0.75 * pulse * g * g;
            }
            else
            {
                const float ds = RoundBox(p - float2(0.0, margin * 0.3), hb, radius);
                const float g = saturate(1.0 - max(ds + margin * 0.15, 0.0) / (margin * 0.85));
                outerA = 0.55 * g * g;
            }
        }
        alpha = body + outerA * (1.0 - body);
        if (alpha <= 0.002) { discard; }
        col = (fill * body + outer * outerA * (1.0 - body)) / alpha;
    }

    return float4(col, alpha * input.color.a);
}
