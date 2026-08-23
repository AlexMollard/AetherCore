// The sigil you gather from, as a UIMaterial ("ui_sigil"). Applied to the element's OWN
// draw and masked to it, so this is a disc cut out of one UI image rather than a quad
// pasted over the interface.
//
// A ring of marks around a pupil that is not quite closed. It breathes on its own, opens
// when struck, and as dread rises the ring rusts, the marks lose their spacing and the
// pupil widens - so the thing you click reports the state of the game without a label.
//
//   params = (time, dread 0..1, punch 0..1, surge)
//   color0 = ichor,  color1 = dread
//
// Reuses the shared UI vertex stage, so it receives the real widget geometry.

[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

#include "include/UiVertexOut.slangh"

struct ShapesPush
{
    float4   screenSize;
    uint64_t commandData;
    uint     pad0;
    uint     pad1;
    float4   params;
    float4   color0;
    float4   color1;
};

[[vk::push_constant]] ShapesPush pc;

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

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    UiClipDiscard(input);

    const float time  = pc.params.x;
    const float dread = saturate(pc.params.y);
    const float punch = saturate(pc.params.z);
    const float surge = saturate(pc.params.w);

    const float2 p = input.local;
    const float r = length(p);
    const float a = atan2(p.y, p.x);

    // Everything outside the disc belongs to the interface behind it.
    if (r > 1.0) { discard; }

    // A slow breath the player never consciously notices, and a fast one that only exists
    // while dread is high - the tell that the thing is awake.
    const float breath = sin(time * 0.9) * 0.012 + sin(time * 3.7) * 0.010 * smoothstep(0.5, 1.0, dread);

    // The colour the whole sigil is mixed toward. Ichor at rest, rust at the brink, and
    // briefly bright on a strike.
    const float3 live = lerp(pc.color0.rgb, pc.color1.rgb, dread * 0.85);
    const float3 hot  = lerp(live, float3(1.0, 0.96, 0.90), punch * 0.55 + surge * 0.15);

    float3 col = float3(0.0, 0.0, 0.0);
    float alpha = 0.0;

    // ── The outer ring ───────────────────────────────────────────────────────────────
    const float ringR = 0.86 + breath + punch * 0.05;
    const float ring = smoothstep(0.055, 0.0, abs(r - ringR));
    col += hot * ring * (0.55 + surge * 0.5);
    alpha = max(alpha, ring);

    // ── The marks ────────────────────────────────────────────────────────────────────
    // Twelve strokes around the ring. Their spacing is perfect at rest and drifts apart as
    // dread rises: the ring stops being a made thing and starts being a growth.
    const float spokes = 12.0;
    const float drift = dread * 0.35;
    const float spoke = frac((a / 6.28318530718) * spokes + vnoise(float2(floor(a * 3.0), 7.0)) * drift);
    const float markBand = smoothstep(0.16, 0.0, abs(r - (0.68 + breath)));
    const float mark = markBand * smoothstep(0.30, 0.0, abs(spoke - 0.5));
    col += hot * mark * 0.9;
    alpha = max(alpha, mark);

    // ── The pupil ────────────────────────────────────────────────────────────────────
    // Widens with dread and snaps open on a strike. The dark inside it is genuinely dark:
    // the one part of the screen with no light in it at all.
    const float pupilR = 0.24 + dread * 0.16 + punch * 0.10;
    const float pupil = smoothstep(pupilR + 0.03, pupilR - 0.03, r);
    const float iris = smoothstep(pupilR + 0.34, pupilR, r) - pupil;

    // The iris is not flat: strands running out from the pupil, turning slowly.
    const float strands = 0.55 + 0.45 * vnoise(float2(a * 6.0 + time * 0.09, r * 9.0 - time * 0.15));
    col += lerp(live * 0.55, hot, punch) * iris * strands;
    alpha = max(alpha, iris * 0.95);

    col = lerp(col, float3(0.010, 0.012, 0.016), pupil);
    alpha = max(alpha, pupil);

    // ── The bloom under it ───────────────────────────────────────────────────────────
    // A faint fill across the whole disc so the sigil sits in its own light rather than on
    // the background, and a rim that catches when it is struck.
    const float fill = smoothstep(1.0, 0.15, r);
    col += live * fill * (0.06 + punch * 0.22 + surge * 0.10);
    alpha = max(alpha, fill * (0.16 + punch * 0.35));

    const float rim = smoothstep(0.02, 0.0, abs(r - 0.99));
    col += hot * rim * (0.25 + punch * 0.7);
    alpha = max(alpha, rim * (0.4 + punch * 0.6));

    // Grain, so the large flat areas do not band.
    col += (hash21(p * 340.0 + time) - 0.5) * 0.015;

    return float4(col, saturate(alpha) * input.color.a);
}
