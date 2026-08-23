// One rite, drawn analytically ("ui_rite", UIMaterial, one element per rite).
//
// These used to be 48x48 hand-drawn sprites. Crisp, but flat: no light on them, no contact
// with the floor, no depth, and a palette of their own that only approximately matched the
// one everything else was built from. Standing on an analytically lit, perspective-projected
// floor they read as stickers pasted over a photograph.
//
// So they are built the same way the floor and the sigil are: signed-distance shapes, shaded
// by one light direction shared with the parish, grounded with a contact shadow, and coloured
// by sampling the SAME ramp. A rite now sits IN the parish rather than on top of it.
//
//   params = (time, riteIndex 0..7, growth 0..1, flare 0..1)
//   color0 = the rite's tint (a step on the ramp)   color1 = ichor, for the core
//
// local space: x in [-1, 1], y in [-1, 1] with y DOWN. The structure stands on y = +1.

// Reuses the shared UI vertex stage, so it receives the real widget geometry - the same
// contract ui_sigil.slang is written against.

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

// ── Signed distance primitives. Negative inside. ───────────────────────────────────
float sdBox(float2 p, float2 b)
{
    const float2 d = abs(p) - b;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}

float sdRound(float2 p, float2 b, float r)
{
    return sdBox(p, b - r) - r;
}

float sdCircle(float2 p, float r) { return length(p) - r; }

// A pointed arch: two circles meeting at a point over a rectangular opening. The single
// shape that says "this is a building somebody worshipped in" more than any other.
float sdArch(float2 p, float2 b, float r)
{
    const float body = sdBox(p - float2(0.0, r * 0.5), float2(b.x, b.y - r * 0.5));
    const float head = sdCircle(p - float2(0.0, -b.y + r), r);
    return min(body, head);
}

// A tapering spire.
float sdSpire(float2 p, float w, float h, float top)
{
    const float t = saturate((p.y + h) / (2.0 * h));
    const float half = lerp(top, w, t);
    return max(abs(p.x) - half, abs(p.y) - h);
}

float opUnion(float a, float b) { return min(a, b); }
float opSub(float a, float b) { return max(a, -b); }

// The ramp, mirrored from scripts/Palette.cs - the same eight steps the floor samples.
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

// ── The eight structures ───────────────────────────────────────────────────────────
// Each returns the distance to its silhouette, and writes out where its core sits so the
// caller can put ichor in it without every shape having to know how the light works.
//
// Told apart by SILHOUETTE, exactly as the palette rules require: no rite carries a hue of
// its own, so the shape is the whole of the identity.
float Structure(int rite, float2 p, float time, out float core, out float detail)
{
    core = 1e5;
    detail = 1e5;
    float d = 1e5;

    if (rite == 0)
    {
        // Grave Lantern: a post with a glass housing and a flame in it.
        d = opUnion(d, sdBox(p - float2(0.0, 0.86), float2(0.42, 0.14)));   // base
        d = opUnion(d, sdBox(p - float2(0.0, 0.55), float2(0.26, 0.20)));   // stem
        d = opUnion(d, sdRound(p - float2(0.0, 0.05), float2(0.40, 0.44), 0.05));
        d = opUnion(d, sdBox(p - float2(0.0, -0.44), float2(0.46, 0.09))); // cap
        d = opUnion(d, sdBox(p - float2(0.0, -0.62), float2(0.06, 0.12))); // hook
        detail = sdBox(p - float2(0.0, 0.05), float2(0.30, 0.34));         // the glass
        core = sdCircle(p - float2(0.0, 0.10), 0.17);
    }
    else if (rite == 1)
    {
        // Bone Choir: three skulls in a row, mouths open.
        for (int i = -1; i <= 1; ++i)
        {
            // Cranium, brow, and a jaw hung below it - three circles read as three balls,
            // which is not what a choir of the dead is supposed to look like.
            const float2 q = p - float2(float(i) * 0.56, 0.22 + abs(float(i)) * 0.20);
            d = opUnion(d, sdRound(q, float2(0.30, 0.32), 0.26));            // cranium
            d = opUnion(d, sdRound(q - float2(0.0, 0.34), float2(0.20, 0.16), 0.07)); // jaw
            d = opUnion(d, sdBox(q - float2(0.0, 0.56), float2(0.09, 0.14))); // neck
            // Sockets and nasal cavity, cut deep.
            detail = opUnion(detail, sdRound(q - float2(-0.13, -0.02), float2(0.10, 0.09), 0.04));
            detail = opUnion(detail, sdRound(q - float2(0.13, -0.02), float2(0.10, 0.09), 0.04));
            detail = opUnion(detail, sdCircle(q - float2(0.0, 0.16), 0.055));
            // The note it is holding, in the open mouth.
            core = opUnion(core, sdRound(q - float2(0.0, 0.36), float2(0.11, 0.05), 0.04));
        }
    }
    else if (rite == 2)
    {
        // Weeping Statue: a bowed figure on a plinth, one tear falling.
        d = opUnion(d, sdBox(p - float2(0.0, 0.90), float2(0.52, 0.10)));   // plinth
        d = opUnion(d, sdBox(p - float2(0.0, 0.74), float2(0.40, 0.10)));
        // Robe: widens toward the ground.
        const float robe = lerp(0.14, 0.36, saturate((p.y + 0.30) / 1.00));
        d = opUnion(d, max(abs(p.x) - robe, abs(p.y - 0.24) - 0.48));
        d = opUnion(d, sdCircle(p - float2(0.0, -0.44), 0.26));             // bowed head
        detail = sdCircle(p - float2(0.0, -0.52), 0.20);                    // cowl shadow
        // The tear, falling on a loop.
        const float fall = frac(time * 0.32);
        core = sdCircle(p - float2(0.10, -0.34 + fall * 0.95), 0.045 + fall * 0.02);
    }
    else if (rite == 3)
    {
        // Flesh Loom: a frame with warp threads and a shuttle crossing it.
        d = opUnion(d, sdBox(p - float2(-0.80, 0.10), float2(0.10, 0.86)));  // uprights
        d = opUnion(d, sdBox(p - float2(0.80, 0.10), float2(0.10, 0.86)));
        d = opUnion(d, sdBox(p - float2(0.0, -0.68), float2(0.88, 0.10)));   // beams
        d = opUnion(d, sdBox(p - float2(0.0, 0.88), float2(0.88, 0.10)));
        // The warp: the threads are part of the SOLID, not a detail cut into an empty
        // frame - carving them out of nothing left the loom reading as a picture frame.
        const float warp = abs(frac(p.x * 4.5 + 0.5) - 0.5) / 4.5;
        const float threads = max(warp - 0.016, abs(p.y - 0.10) - 0.74);
        d = opUnion(d, threads);
        // A few crossed wefts already woven, near the bottom.
        d = opUnion(d, max(abs(p.y - 0.62) - 0.020, abs(p.x) - 0.80));
        d = opUnion(d, max(abs(p.y - 0.74) - 0.020, abs(p.x) - 0.80));
        detail = max(abs(p.y - 0.68) - 0.05, abs(p.x) - 0.78);
        // The shuttle runs back and forth across the warp, trailing the thread it is laying.
        const float pass = sin(time * 1.1) * 0.62;
        core = sdRound(p - float2(pass, 0.10), float2(0.15, 0.06), 0.05);
        core = opUnion(core, max(abs(p.y - 0.10) - 0.014, abs(p.x) - abs(pass)));
    }
    else if (rite == 4)
    {
        // Ossuary Engine: a ribbed housing on a bed, with a furnace and a stack.
        d = opUnion(d, sdBox(p - float2(0.0, 0.86), float2(0.92, 0.14)));    // bed
        d = opUnion(d, sdRound(p - float2(0.0, 0.18), float2(0.74, 0.56), 0.06));
        d = opUnion(d, sdBox(p - float2(-0.48, -0.62), float2(0.15, 0.24))); // stack
        // Ribs banding the housing, cut as deep grooves so they catch the light along
        // their edges rather than smearing into a gradient across the whole face.
        const float rib = abs(frac(p.y * 4.0 + 0.5) - 0.5) / 4.0;
        detail = max(rib - 0.030, sdBox(p - float2(0.0, 0.18), float2(0.66, 0.48)));
        // A hatch on the front, and the bolts round it.
        detail = opUnion(detail, sdRound(p - float2(-0.28, 0.22), float2(0.22, 0.26), 0.05));
        // The furnace, breathing.
        const float breath = 0.20 + sin(time * 2.3) * 0.02;
        core = sdCircle(p - float2(0.30, 0.20), breath);
    }
    else if (rite == 5)
    {
        // Drowned Chapel: a pitched roof and a cross over a lit window, half under water.
        d = opUnion(d, sdBox(p - float2(0.0, 0.42), float2(0.62, 0.52)));    // nave
        // The roof: a triangle, as the half-plane intersection of two slopes.
        const float roof = max(max(p.y - (-0.10), -p.y - 0.86),
                               abs(p.x) * 1.30 + p.y + 0.10);
        d = opUnion(d, roof);
        d = opUnion(d, sdBox(p - float2(0.0, -0.86), float2(0.05, 0.16)));   // cross
        d = opUnion(d, sdBox(p - float2(0.0, -0.90), float2(0.14, 0.05)));
        detail = sdArch(p - float2(0.0, 0.44), float2(0.20, 0.30), 0.20);    // window frame
        core = sdArch(p - float2(0.0, 0.44), float2(0.14, 0.24), 0.14);
        // The waterline, rocking. Everything under it is cut away into the dark.
        const float water = 0.72 + sin(time * 0.7 + p.x * 2.0) * 0.02;
        detail = opUnion(detail, max(abs(p.y - water) - 0.020, abs(p.x) - 1.0));
    }
    else if (rite == 6)
    {
        // Pale Shepherd: a tall figure with a crook, and its flock at its feet.
        const float robe = lerp(0.13, 0.30, saturate((p.y + 0.20) / 1.05));
        d = opUnion(d, max(abs(p.x + 0.18) - robe, abs(p.y - 0.32) - 0.56));
        d = opUnion(d, sdCircle(p - float2(-0.18, -0.42), 0.23));            // head
        d = opUnion(d, sdBox(p - float2(0.52, 0.10), float2(0.045, 0.84)));  // crook
        d = opUnion(d, abs(sdCircle(p - float2(0.40, -0.66), 0.17)) - 0.045);
        d = opUnion(d, sdCircle(p - float2(-0.72, 0.80), 0.19));             // flock
        d = opUnion(d, sdCircle(p - float2(-0.44, 0.87), 0.15));
        detail = sdCircle(p - float2(-0.18, -0.46), 0.16);
        core = sdCircle(p - float2(-0.24, -0.46), 0.055);                    // the eye
    }
    else
    {
        // Hollow Mouth: a ring of teeth around a throat that goes nowhere.
        d = abs(sdCircle(p, 0.72)) - 0.16;
        // Teeth: wedges pointing inward, all the way round.
        const float a = atan2(p.y, p.x);
        const float r = length(p);
        const float tooth = abs(frac(a * (16.0 / 6.28318530718) + 0.5) - 0.5);
        d = opUnion(d, max(tooth * r * 2.6 - 0.075, abs(r - 0.50) - 0.22));
        detail = sdCircle(p, 0.26);
        core = sdCircle(p, 0.10 + sin(time * 1.7) * 0.012);
    }
    return d;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    UiClipDiscard(input);

    const float time   = pc.params.x;
    const int   rite   = int(pc.params.y + 0.5);
    const float growth = saturate(pc.params.z);
    const float flare  = saturate(pc.params.w);

    // local is -1..1 with y down. The structures are authored to stand on y = +1, but the
    // rect's own edge is there too, which sliced off every base and every contact shadow.
    // Squeezing shape space into the top 93% leaves a strip underneath for the shadow.
    const float2 p = input.local;
    const float ground = 0.86;
    const float fit = (1.0 + ground) * 0.5;
    const float2 q = float2(p.x / fit, (p.y + 1.0) / fit - 1.0);

    float core, detail;
    const float d = Structure(rite, q, time, core, detail) * fit;

    // Antialiasing width from the actual screen derivative, so the edge is one pixel at any
    // size the rite is drawn at - which is the whole reason this is not a sprite any more.
    const float aa = fwidth(d) * 0.9 + 1e-5;
    const float solid = smoothstep(aa, -aa, d);

    // ── The contact shadow ───────────────────────────────────────────────────────────
    // A soft pool where the structure meets the floor. Without this the rite floats no
    // matter how exactly its feet are placed, because nothing on the floor changes.
    const float pool = smoothstep(ground - 0.30, ground, p.y)
                     * smoothstep(1.0, ground, p.y)
                     * smoothstep(0.92, 0.10, abs(p.x));

    if (solid < 0.004 && pool < 0.004)
    {
        discard;
    }

    // ── Shading ──────────────────────────────────────────────────────────────────────
    // One light, high and to the left, shared with the parish's own sense of where the
    // light is. The gradient of the distance field IS the surface normal, so the shape
    // shades itself without anyone painting highlights into it by hand.
    const float2 grad = normalize(float2(ddx(d), ddy(d)) + 1e-6);
    const float2 lightDir = normalize(float2(-0.55, -1.0));
    const float lambert = saturate(dot(grad, -lightDir) * 0.5 + 0.5);

    // Ambient occlusion from depth into the shape: interiors sit in their own shadow.
    const float depth = saturate(-d * 3.2);
    const float ao = lerp(1.0, 0.42, depth);

    // The rite's tint is a step on the ramp with no hue in it, so the whole structure is
    // built out of luminance and the ONLY colour on it is the core.
    const float key = dot(pc.color0.rgb, float3(0.299, 0.587, 0.114));
    float tone = key * (0.55 + lambert * 1.05) * ao;

    // Recessed detail - glass, ribs, sockets, the warp - reads as cut into the structure.
    tone *= 1.0 - smoothstep(aa, -aa, detail * fit) * 0.50;

    // Stonework grain, so a large face is not a flat fill.
    tone *= 0.94 + hash21(floor(p * 90.0)) * 0.12;

    float3 col = Ramp(tone);

    // A rim of light along the top edge, which is what separates a silhouette from the
    // fog behind it at this distance.
    const float rim = smoothstep(aa * 3.0, 0.0, abs(d)) * saturate(-grad.y);
    col = lerp(col, Ramp(0.92), rim * 0.42);

    // ── The core ─────────────────────────────────────────────────────────────────────
    // The one coloured thing on the rite, and it brightens as the rite works and flares
    // when it delivers - so what the eye is drawn to is exactly what is paying out.
    const float ca = fwidth(core * fit) + 1e-5;
    core *= fit;
    const float coreMask = smoothstep(ca, -ca, core);
    const float3 ichor = pc.color1.rgb;
    col = lerp(col, ichor * (0.85 + flare * 0.6), coreMask);
    // Its glow spilling onto the surrounding stone.
    const float bleed = exp(-max(core, 0.0) * 9.0) * (0.30 + growth * 0.25 + flare * 0.85);
    col += ichor * bleed * 0.42 * solid;

    // The flare washes the whole structure, briefly.
    col = lerp(col, Ramp(0.90), flare * 0.22);

    // ── Composite ────────────────────────────────────────────────────────────────────
    // The shadow is the structure's own darkness spread on the floor, so it composites
    // under the shape rather than being another thing drawn beside it.
    float3 outCol = lerp(float3(0.0, 0.0, 0.0), col, solid);
    float outA = max(solid, pool * 0.60 * (1.0 - solid));

    return float4(outCol, outA * input.color.a);
}
