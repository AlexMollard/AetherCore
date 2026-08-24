// One relic, drawn analytically ("ui_relic", UIMaterial, one element per relic shown).
//
// Relics are generated from a seed, endlessly, so they cannot be sprites - there is no
// artist and no atlas for a supply with no upper bound. They are built the way the rites and
// the sigil are: signed-distance shapes, one shared grey ramp, and accent colour used only
// where it means something.
//
// WHAT THE GRADE DOES. A Leavings is a bare silhouette in ramp grey. Every grade above it
// earns another layer - a bevel, then a setting, then rays, then a living core and a rim
// that will not sit still - and takes a little more accent colour. So a common thing cannot
// look like a legendary one: the difference is how much is THERE, not which palette entry it
// was assigned. That also keeps the parish's one rule intact, that a coloured pixel means
// something, because the colour arrives in step with the rarity it is reporting.
//
//   params = (time, seed, grade 0..4, glow 0..1)
//   color0 = ichor, for cores and accents   color1 = dread, for the top grade's rim
//
// local space: x in [-1, 1], y in [-1, 1] with y DOWN. The relic is centred; unlike a rite
// it stands on nothing, because it is being held rather than being visited.

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

// ── Small helpers ────────────────────────────────────────────────────────────────────
// Local copies rather than a shared include, matching how ui_rite.slang and ui_sigil.slang
// each carry their own: these are four-line functions, and a header shared between shaders
// that are otherwise independent is a coupling that buys nothing.

float hash11(float n)
{
    return frac(sin(n * 127.1) * 43758.5453);
}

float hash21(float2 p)
{
    return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}

float sdCircle(float2 p, float r)
{
    return length(p) - r;
}

float sdBox(float2 p, float2 b)
{
    const float2 d = abs(p) - b;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}

float sdRound(float2 p, float2 b, float r)
{
    return sdBox(p, b - r) - r;
}

float sdSeg(float2 p, float2 a, float2 b, float r)
{
    const float2 pa = p - a;
    const float2 ba = b - a;
    const float h = saturate(dot(pa, ba) / dot(ba, ba));
    return length(pa - ba * h) - r;
}

// A ring: the shell of a circle rather than its interior.
float sdRing(float2 p, float r, float w)
{
    return abs(length(p) - r) - w;
}

float opUnion(float a, float b) { return min(a, b); }
float opSub(float a, float b)   { return max(a, -b); }

float2 rot(float2 p, float a)
{
    const float s = sin(a);
    const float c = cos(a);
    return float2(p.x * c - p.y * s, p.x * s + p.y * c);
}

// The one grey ramp the whole game is built from. Copied rather than shared for the same
// reason as the helpers above, and it must stay in step with Palette.cs.
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

// ── The body ─────────────────────────────────────────────────────────────────────────
// Six silhouettes, chosen by the seed. Told apart by SHAPE alone, exactly as the rites are:
// no relic carries a hue of its own, so what it looks like is the whole of its identity.

float Body(int kind, float2 p, out float core)
{
    core = 0.0;
    float d = 1.0;

    if (kind == 0)
    {
        // A bead, drilled through.
        d = sdCircle(p, 0.52);
        d = opSub(d, sdCircle(p, 0.14));
        core = 1.0 - saturate(length(p) / 0.52);
    }
    else if (kind == 1)
    {
        // A tally, or a locket: a plate with a shoulder.
        d = sdRound(p, float2(0.40, 0.54), 0.10);
        d = opUnion(d, sdRound(p - float2(0.0, -0.58), float2(0.16, 0.10), 0.06));
        core = 1.0 - saturate(abs(p.x) / 0.40);
    }
    else if (kind == 2)
    {
        // A vial, stoppered.
        d = sdRound(p - float2(0.0, 0.10), float2(0.30, 0.44), 0.20);
        d = opUnion(d, sdRound(p - float2(0.0, -0.50), float2(0.16, 0.14), 0.05));
        core = smoothstep(0.55, -0.10, p.y) * (1.0 - saturate(abs(p.x) / 0.30));
    }
    else if (kind == 3)
    {
        // A nail, or a pin.
        d = sdSeg(p, float2(0.0, -0.40), float2(0.0, 0.66), 0.075);
        d = opUnion(d, sdRound(p - float2(0.0, -0.48), float2(0.26, 0.09), 0.05));
        core = 1.0 - saturate(abs(p.x) / 0.10);
    }
    else if (kind == 4)
    {
        // A key: a bow, a shaft and two teeth.
        d = sdRing(p - float2(0.0, -0.42), 0.26, 0.075);
        d = opUnion(d, sdSeg(p, float2(0.0, -0.16), float2(0.0, 0.62), 0.070));
        d = opUnion(d, sdBox(p - float2(0.16, 0.44), float2(0.16, 0.065)));
        d = opUnion(d, sdBox(p - float2(0.14, 0.62), float2(0.14, 0.060)));
        core = 1.0 - saturate(abs(p.x) / 0.12);
    }
    else
    {
        // A ring, plain, with a single setting.
        d = sdRing(p, 0.46, 0.10);
        d = opUnion(d, sdCircle(p - float2(0.0, -0.46), 0.15));
        core = smoothstep(0.30, 0.0, length(p - float2(0.0, -0.46)));
    }

    return d;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    UiClipDiscard(input);

    const float time  = pc.params.x;
    const float seed  = pc.params.y;
    const int   grade = int(saturate(pc.params.z / 4.0) * 4.0 + 0.5);
    const float glow  = saturate(pc.params.w);

    // Every choice below is drawn from its own hash of the seed, so adding a feature later
    // cannot shift the ones already rolled - the same discipline the C# generator keeps, and
    // for the same reason: a relic must look the same tomorrow as it did when it was found.
    const int   kind  = int(hash11(seed * 1.7 + 3.0) * 6.0) % 6;
    const float tilt  = (hash11(seed * 2.3 + 11.0) - 0.5) * 0.55;
    const float notch = hash11(seed * 3.1 + 19.0);
    const float spin  = (hash11(seed * 5.9 + 31.0) - 0.5) * 2.0;

    // The top grade will not sit still. Everything below it is static, because a common
    // thing that moves reads as more important than a rare thing that does not.
    const float drift = grade >= 4 ? sin(time * 0.6 + seed) * 0.06 : 0.0;
    const float2 p = rot(input.local * 1.28, tilt + drift * 0.4);

    float core;
    float d = Body(kind, p, core);

    // ── What the grade adds ──────────────────────────────────────────────────────────
    // Layers, not louder colours. Each one is another thing present in the silhouette, so
    // the difference between grades survives being seen small and in grey.

    if (grade >= 1)
    {
        // A notch or a bead worked into the body: the mark of something kept deliberately.
        const float2 n = p - float2(0.0, 0.36 - notch * 0.72);
        d = opSub(d, sdCircle(n, 0.055 + notch * 0.03));
    }

    float setting = 1.0;
    if (grade >= 2)
    {
        // A setting around it, turned off the body's own angle.
        setting = sdRing(rot(p, spin * 0.6), 0.74, 0.035);
        d = opUnion(d, setting);
    }

    if (grade >= 3)
    {
        // Rays. Four of them, struck through the setting.
        const float2 r = rot(p, spin * 0.6 + 0.785398);
        float rays = sdSeg(r, float2(-0.94, 0.0), float2(-0.62, 0.0), 0.028);
        rays = opUnion(rays, sdSeg(r, float2(0.62, 0.0), float2(0.94, 0.0), 0.028));
        rays = opUnion(rays, sdSeg(r, float2(0.0, -0.94), float2(0.0, -0.62), 0.028));
        rays = opUnion(rays, sdSeg(r, float2(0.0, 0.62), float2(0.0, 0.94), 0.028));
        d = opUnion(d, rays);
    }

    const float aa = fwidth(d) * 0.9 + 1e-5;
    const float solid = smoothstep(aa, -aa, d);

    // A halo, and only for the things that have earned one.
    const float halo = grade >= 2
        ? smoothstep(1.15, 0.35, length(p)) * (0.05 + 0.05 * float(grade - 2))
        : 0.0;

    if (solid < 0.004 && halo < 0.004)
    {
        discard;
    }

    // ── Colour ───────────────────────────────────────────────────────────────────────
    // Structure comes off the ramp, always. Accent arrives in proportion to the grade, so a
    // Leavings is bare grey and only a Hollowed thing is properly lit from inside.

    const float3 ichor = pc.color0.rgb;
    const float3 dread = pc.color1.rgb;

    // Shaded by the same one light direction the parish uses: up and to the left.
    const float lit = saturate(0.55 + 0.45 * dot(normalize(float2(-0.55, -0.8)), normalize(p + 1e-4)));
    float3 col = Ramp(0.26 + lit * 0.34 + core * 0.10);

    const float accent = float(grade) / 4.0;
    col = lerp(col, ichor, core * accent * (0.35 + glow * 0.35));

    if (grade >= 2)
    {
        // The setting catches the light rather than being painted.
        const float edge = smoothstep(0.035, 0.0, abs(setting));
        col = lerp(col, ichor, edge * (0.25 + accent * 0.35));
    }

    if (grade >= 4)
    {
        // The rim that will not settle. Dread, and the only place this shader uses it: the
        // top grade is the one where the thing you are holding has been got at.
        const float rim = smoothstep(0.06, 0.0, abs(length(p) - (0.86 + drift)));
        col = lerp(col, dread, rim * 0.55);
    }

    float alpha = solid;
    col += ichor * halo * (0.5 + glow * 0.5);
    alpha = max(alpha, halo);

    // Grain, so the flat interiors do not band at large sizes.
    col += (hash21(p * 260.0 + time * 0.05) - 0.5) * 0.012;

    return float4(col, saturate(alpha) * input.color.a);
}
