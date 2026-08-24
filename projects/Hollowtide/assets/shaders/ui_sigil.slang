// The sigil you gather from, as a UIMaterial ("ui_sigil"). Applied to the element's OWN
// draw and masked to it, so this is a disc cut out of one UI image rather than a quad
// pasted over the interface.
//
// WHAT IT IS. A mark cut into stone. Not an eye - the previous version was an iris around a
// pupil, and an eye in a dark game reads as a portal watching you, which is somebody else's
// story. This is a parish: things here are carved, worn, and worked with hands. So the sigil
// is a weathered disc with a ritual mark GROOVED into it, and striking it brings ichor up out
// of the cuts the way pressing a thumb into a wound brings blood.
//
// Everything it reports, it reports through that one idea:
//   - at rest     a slow tide breathes along the channels, and an outer ring of teeth turns
//   - struck      a front leaves the centre and travels out, the grooves flood behind it,
//                 and embers lift off the cuts as the flood drains back
//   - dread       the stone sours, the rim chips, hairline cracks open, and the whole mark
//                 begins to tremble
//   - surge       the carving rings, a bright pulse travelling out along the channels
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

// ── Helpers ──────────────────────────────────────────────────────────────────────────
// Local copies, as in ui_rite and ui_relic: four-line functions shared between otherwise
// independent shaders would be a coupling that buys nothing.

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
    const float a = hash21(i);
    const float b = hash21(i + float2(1.0, 0.0));
    const float c = hash21(i + float2(0.0, 1.0));
    const float d = hash21(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float fbm(float2 p)
{
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; i++)
    {
        sum += vnoise(p) * amp;
        p *= 2.03;
        amp *= 0.5;
    }
    return sum;
}

float sdSeg(float2 p, float2 a, float2 b, float r)
{
    const float2 pa = p - a;
    const float2 ba = b - a;
    const float h = saturate(dot(pa, ba) / dot(ba, ba));
    return length(pa - ba * h) - r;
}

// The one grey ramp the whole game is built from. Must stay in step with Palette.cs.
float3 Ramp(float t)
{
    const float3 ink   = float3(0.027, 0.031, 0.043);
    const float3 slate = float3(0.082, 0.094, 0.122);
    const float3 stone = float3(0.137, 0.157, 0.200);
    const float3 ash   = float3(0.227, 0.255, 0.314);
    const float3 bone  = float3(0.431, 0.463, 0.525);
    const float3 pale  = float3(0.659, 0.690, 0.745);
    t = saturate(t);
    if (t < 0.20) { return lerp(ink,   slate, t / 0.20); }
    if (t < 0.40) { return lerp(slate, stone, (t - 0.20) / 0.20); }
    if (t < 0.60) { return lerp(stone, ash,   (t - 0.40) / 0.20); }
    if (t < 0.80) { return lerp(ash,   bone,  (t - 0.60) / 0.20); }
    return lerp(bone, pale, (t - 0.80) / 0.20);
}

// ── The cut mark ─────────────────────────────────────────────────────────────────────
// Returns the distance to the engraved channels. Everything is grooves: rings, spokes and
// the mark at the centre, so one distance field lights the whole carving and one flood of
// ichor fills all of it.

float Channels(float2 p, float time, float dread)
{
    // At high dread the whole mark trembles - a small, fast, unhappy motion, distinct from
    // the slow tide below it. Nothing else in the game shakes at the keeper except a
    // visitation, so this reads as the stone itself being uneasy rather than as the camera.
    const float unease = smoothstep(0.55, 1.0, dread);
    p += float2(sin(time * 27.0), cos(time * 23.0)) * 0.004 * unease;

    const float r = length(p);
    const float a = atan2(p.y, p.x);

    // Two concentric rings, the outer one heavier.
    float d = abs(r - 0.74) - 0.020;
    d = min(d, abs(r - 0.50) - 0.013);

    // Six spokes between the rings, struck from the centre outward. Six rather than eight so
    // the face keeps large uncarved areas - a mark needs stone left around it to read as cut
    // INTO something rather than as a wire frame floating in the dark.
    const float sector = 3.14159265 / 3.0;
    const float folded = abs(fmod(a + sector * 0.5 + 6.2831853, sector) - sector * 0.5);
    const float2 spoke = float2(cos(folded), sin(folded)) * r;
    d = min(d, sdSeg(spoke, float2(0.22, 0.0), float2(0.74, 0.0), 0.016));

    // The mark at the middle: a small cross, turned, with a bored hollow at its heart. Not a
    // circle inside a circle - that is the shape that read as a pupil.
    const float tilt = 0.5235988;
    const float2 q = float2(p.x * cos(tilt) - p.y * sin(tilt), p.x * sin(tilt) + p.y * cos(tilt));
    d = min(d, sdSeg(q, float2(-0.19, 0.0), float2(0.19, 0.0), 0.018));
    d = min(d, sdSeg(q, float2(0.0, -0.19), float2(0.0, 0.19), 0.018));
    d = min(d, abs(r - 0.075) - 0.014);

    // An outer ring of teeth, turning slowly against the carving. A mechanism, not decoration:
    // the sigil is a thing that WORKS, and something that never moves at rest reads as a
    // picture of a sigil rather than one that is running.
    const float teeth = 24.0;
    const float turn = time * 0.16;
    const float toothed = abs(fmod(a + turn + 6.2831853 * 4.0, 6.2831853 / teeth) - 3.14159265 / teeth);
    const float toothMask = smoothstep(0.06, 0.02, toothed);
    d = min(d, max(abs(r - 0.875) - 0.026, -toothMask + 0.5));

    // Dread opens hairline cracks across the face. They are cut too, so they fill with ichor
    // like everything else - the parish does not distinguish between a channel somebody carved
    // and one that opened on its own.
    if (dread > 0.25)
    {
        const float crack = abs(fbm(p * 3.2 + 11.0) - 0.5) * 2.0;
        const float bite = smoothstep(0.25, 1.0, dread);
        d = min(d, (crack - 0.035 * bite) * 0.22);
    }

    return d;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    UiClipDiscard(input);

    const float time  = pc.params.x;
    const float dread = saturate(pc.params.y);
    const float punch = saturate(pc.params.z);
    const float surge = saturate(pc.params.w);

    const float3 ichor = pc.color0.rgb;
    const float3 dreadC = pc.color1.rgb;

    const float2 p = input.local;
    const float r = length(p);

    // ── The disc ─────────────────────────────────────────────────────────────────────
    // A worn edge rather than a struck circle: the rim is chewed by noise, and dread chews
    // it harder, so the stone looks older the closer the dark gets.
    const float chew = (fbm(p * 5.0 + 3.0) - 0.5) * (0.020 + dread * 0.030);
    const float edge = r - (0.965 + chew);
    const float aa = fwidth(edge) * 0.9 + 1e-5;
    const float disc = smoothstep(aa, -aa, edge);

    if (disc < 0.004)
    {
        discard;
    }

    // ── Stone ────────────────────────────────────────────────────────────────────────
    // Lit from up and to the left, the same direction the parish uses. Grain from the same
    // noise that chewed the rim, so the surface and its edge belong to one block.
    const float grain = fbm(p * 7.5 + 1.7);
    const float2 lightDir = normalize(float2(-0.55, -0.80));
    const float dome = saturate(0.55 + 0.45 * dot(lightDir, p / max(r, 1e-4)));

    float3 col = Ramp(0.20 + dome * 0.26 + grain * 0.12);
    // Soured toward dread as it rises, but never far: the stone is structure, and structure
    // in this game does not take a hue of its own. See Palette.cs.
    col = lerp(col, lerp(col, dreadC, 0.32), dread * dread);

    // The bevel where the face meets the rim, so the disc has thickness.
    const float bevel = smoothstep(0.965, 0.86, r);
    col *= 0.55 + 0.45 * bevel;

    // ── The carving ──────────────────────────────────────────────────────────────────
    const float channel = Channels(p, time, dread);
    const float cut = smoothstep(0.012, -0.006, channel);

    // A groove is dark at the bottom and catches light on the side facing the lamp. Offsetting
    // the field along the light direction and differencing it is the cheapest way to get that,
    // and it is what makes this read as CUT rather than as painted-on lines.
    const float lip = Channels(p + lightDir * 0.014, time, dread);
    const float catches = saturate((lip - channel) * 26.0) * cut;

    col = lerp(col, col * 0.28, cut);
    col += Ramp(0.86) * catches * 0.55;

    // ── Ichor in the cuts ────────────────────────────────────────────────────────────
    // Struck, the channels flood from the middle outward and drain back. This is the whole
    // gesture of the game in one effect: you press the stone, and what is underneath comes up.
    //
    // The punch arrives at 1 and decays, so 1 - punch is a radius leaving the centre. That
    // gives a FRONT with a position rather than a brightness that merely fades everywhere at
    // once - which is the difference between the stone carrying something outward and the
    // stone simply flashing.
    const float front = 1.0 - punch;
    const float crest = smoothstep(0.30, 0.0, abs(r - front)) * smoothstep(0.0, 0.15, punch);
    const float wave = saturate(punch * 1.9 - r * 0.85);

    // At rest the channels are never quite still: a slow tide runs along them, strongest at
    // the centre. Idle motion is what stops a thing looking switched off.
    const float tide = 0.5 + 0.5 * sin(time * 1.15 - r * 3.4);
    const float resting = (0.085 + 0.14 * dread) * (0.72 + 0.28 * tide);

    const float flood = saturate(resting + wave * 1.5 + crest * 0.9 + surge * 0.35);

    col = lerp(col, ichor, cut * flood);
    // The glow the flooded channels throw onto the stone beside them.
    col += ichor * cut * flood * 0.5;
    col += ichor * smoothstep(0.055, 0.0, abs(channel)) * flood * 0.35;

    // ── Embers ───────────────────────────────────────────────────────────────────────
    // Motes lifting off the carving while the flood drains back. They rise and fade, so the
    // strike has an aftermath instead of simply ending - the eye keeps something to follow
    // once the front has gone.
    if (punch > 0.02)
    {
        const float2 cell = floor(p * 9.0);
        const float seed = hash21(cell);
        const float rise = frac(seed * 7.3 + time * 0.9);
        const float2 local = frac(p * 9.0) - float2(0.5, 0.5 - rise * 0.55);
        const float mote = smoothstep(0.16, 0.0, length(local));
        // Only where there is a channel to lift off, and only while the strike is settling.
        const float lit = smoothstep(0.05, -0.01, channel) * (1.0 - rise) * punch;
        col += ichor * mote * lit * 0.85 * step(0.55, seed);
    }

    // ── The ring, when a surge is on ─────────────────────────────────────────────────
    // A bright front travelling out along the carving, so a communion is visible on the one
    // thing the keeper is already looking at.
    if (surge > 0.0)
    {
        const float front = frac(time * 0.55);
        const float ring = smoothstep(0.10, 0.0, abs(r - front));
        col += ichor * ring * surge * 0.45 * cut;
    }

    // ── Sitting in its own light ─────────────────────────────────────────────────────
    const float fill = smoothstep(1.0, 0.30, r);
    col += ichor * fill * (0.015 + punch * 0.10 + surge * 0.04);

    // The rim catches hard and briefly on a strike - a fast attack against the slower drain
    // in the channels, so the moment of contact is sharp and what follows is not.
    const float snap = punch * punch;
    const float rim = smoothstep(0.03, 0.0, abs(edge));
    col = lerp(col, lerp(Ramp(0.72), ichor, saturate(punch * 0.9)), rim * (0.30 + snap * 0.65));

    // Grain, so the large flat stone does not band.
    col += (hash21(p * 320.0 + time) - 0.5) * 0.014;

    float alpha = disc;
    alpha = max(alpha, cut * disc);
    return float4(col, saturate(alpha) * input.color.a);
}
