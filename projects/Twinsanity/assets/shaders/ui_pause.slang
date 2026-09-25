// The Twinsanity pause menu's code-drawn backdrop ("ui_pause", UIMaterial).
//
// The swirl backdrop, the three counter discs, the menu ball and the gem track are drawn by
// the PS2 game as coloured geometry: no texture for any of them exists in any archive on the
// disc (tw-extract walked them all). They are rebuilt here as analytic shapes measured on
// 35 fps rig captures of the original menu (logs/pause/rig/, scan lines and absolute-labelled
// crops of the idle frame t00334, 640x480 PAL). All distances below are in those frame pixels,
// relative to each element's own box centre (TwinsanityPause.cs places the boxes):
//   swirl  - ellipse bands about (326,236): outer radii (227,208) with a thin white rim, the outer
//            blue band down to normalised radius 0.79 (y 30..74 on top, x 494..541 on the right),
//            the white band to 0.52 (inner edges at (419,170) and (400,151)), inner blue inside;
//            plus the inner blue's "hook" under the 0%, a disc about (465,290) r=62;
//   disc   - a counter disc: graded blue fill lit from the top left, white rim from r=0.90;
//   ball   - the menu ball, same construction, rim from 0.93;
//   track  - the gem track: a cyan band r=127..167 with white edging (122..172) on the
//            least-squares circle through the six gem centres, (428.9,204.2) r=146, cut at
//            -88 and +74 degrees.
// Colours are the rig's on-screen values (the whole menu reads at ~0.75 brightness on the PS2:
// its white is 187).
//
// params = (time, shape, 0, 0) with shape: 0 swirl, 1 disc, 2 ball, 3 track.
// color0/color1 are a disc's or the ball's lit and shaded fill. The element's image alpha
// (input.color.a) fades the whole shape, so C# fades the menu in and out.
//
// ponytail: the swirl's spiral is approximated by concentric ellipse bands plus one hook disc;
// the real S-curl where the white band tucks into the badge and where the outer band feeds the
// counter discs differs by a few pixels of silhouette. The upgrade path is an arc-length spiral
// SDF fitted to the scan lines in logs/pause/.

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

static const float3 SWIRL_BLUE = float3(0.110, 0.471, 0.733); // rig 28,120,187 (outer band)
static const float3 INNER_BLUE = float3(0.063, 0.420, 0.733); // rig 16,107,187
static const float3 PALE       = float3(0.733, 0.733, 0.733); // rig 187,187,187 (white bands)
static const float3 CYAN       = float3(0.220, 0.733, 0.655); // rig 56,187,167 (gem track)

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    UiClipDiscard(input);

    const int shape = int(pc.params.y + 0.5);
    float alpha = 0.0;
    float3 col = SWIRL_BLUE;

    if (shape == 0)
    {
        // The swirl, in frame pixels about its centre (326,236); the box is 462x424.
        const float2 p = input.local * float2(231.0, 212.0);
        const float e = length(p / float2(227.0, 208.0));
        if (e > 1.02) { discard; }
        if (e > 1.0)        { col = PALE; }        // outer white rim
        else if (e > 0.79)  { col = SWIRL_BLUE; }  // outer blue band
        else if (e > 0.52)  { col = PALE; }        // white band
        else                { col = INNER_BLUE; }  // inner blue (under the badge)
        // The inner blue's hook, carrying the 0%, over the white band at the lower right.
        if (e <= 0.79 && length(p - float2(139.0, 54.0)) < 62.0) { col = INNER_BLUE; }
        alpha = smoothstep(1.02, 1.005, e);
    }
    else if (shape == 1 || shape == 2)
    {
        // A counter disc / the menu ball: white rim, graded fill lit from the top left.
        const float d = length(input.local);
        if (d > 1.0) { discard; }
        const float rim = shape == 2 ? 0.93 : 0.90;
        const float lit = saturate(0.5 - 0.5 * (input.local.x + input.local.y));
        col = d > rim ? PALE : lerp(pc.color1.rgb, pc.color0.rgb, lit);
        alpha = smoothstep(1.0, 0.97, d);
    }
    else
    {
        // The gem track: its 188x342 box is centred at (509,201); p is relative to the gem circle.
        const float2 p = input.local * float2(94.0, 171.0) + float2(509.0 - 428.9, 201.0 - 204.2);
        const float r = length(p);
        const float a = degrees(atan2(p.y, p.x));
        if (a < -88.0 || a > 74.0 || r < 122.0 || r > 172.0) { discard; }
        col = (r < 127.0 || r > 167.0) ? PALE : CYAN;
        alpha = smoothstep(172.0, 170.5, r) * smoothstep(122.0, 123.5, r);
    }

    return float4(col, alpha * input.color.a);
}
