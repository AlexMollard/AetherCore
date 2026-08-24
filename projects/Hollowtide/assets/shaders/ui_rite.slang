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

// A capsule from a to b. Limbs are capsules; nothing else describes an arm as cheaply.
float sdSeg(float2 p, float2 a, float2 b, float r)
{
    const float2 pa = p - a;
    const float2 ba = b - a;
    const float h = saturate(dot(pa, ba) / max(dot(ba, ba), 1e-6));
    return length(pa - ba * h) - r;
}

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
        // Weeping Statue: a hooded figure with its face in its hands, on a stepped plinth.
        //
        // Two earlier attempts came out as a chess pawn, and both failed the same way: the
        // robe was wider than the body at every height, so the shoulders, the arms and the
        // head were all swallowed into one bell. A figure needs a WAIST - something narrower
        // than what is above and below it - and it needs limbs that leave the silhouette.
        // Hands at the face put the elbows outside the body and cut two clear voids through
        // it, which no object shape can imitate.
        d = opUnion(d, sdBox(p - float2(0.0, 0.94), float2(0.60, 0.06)));   // plinth, stepped
        d = opUnion(d, sdBox(p - float2(0.0, 0.84), float2(0.48, 0.06)));
        d = opUnion(d, sdBox(p - float2(0.0, 0.74), float2(0.38, 0.05)));

        // The robe. Narrow through the torso, flaring only below the waist, with a slow
        // ripple down the hem so the edge reads as cloth rather than as a cone.
        const float waist = 0.185 + smoothstep(0.20, 0.70, p.y) * 0.245
                          + sin(p.y * 8.0 + 1.1) * 0.014;
        d = opUnion(d, max(abs(p.x) - waist, abs(p.y - 0.24) - 0.48));

        // Shoulders, square over the hips.
        //
        // These used to march rightward as they rose - robe at 0.02, shoulders 0.05, neck
        // 0.07, head 0.10 - on the theory that offsetting the upper body would read as a bow.
        // It does not. Against a plinth that is dead centre and a pair of steps that are
        // visibly level, a stack of small offsets reads as one thing: the statue is not
        // straight. The bow now lives entirely in the head - lower, and hooded so the face is
        // buried - which is where a viewer looks for it anyway.
        d = opUnion(d, sdRound(p - float2(0.0, -0.24), float2(0.27, 0.09), 0.09));
        d = opUnion(d, sdSeg(p, float2(0.0, -0.30), float2(0.02, -0.40), 0.075)); // neck

        // The head, bowed, under a hood that overhangs the face. The last 0.03 of lean is the
        // whole tilt: enough that it is not standing to attention, small enough to read as a
        // bowed head rather than as a crooked carving.
        const float2 h = p - float2(0.03, -0.55);
        d = opUnion(d, sdCircle(h, 0.185));
        d = opUnion(d, opSub(sdCircle(h - float2(0.0, -0.02), 0.245), sdCircle(h - float2(0.05, 0.10), 0.215)));

        // Arms up to the face: shoulder to elbow held wide, elbow back in to the hands. The
        // elbows sit ABOVE the shoulders, which is what makes the pose read as grief.
        // Symmetric about the body's axis now, where they used to be centred on the offset
        // shoulders and so sat off-centre against the plinth with everything else.
        d = opUnion(d, sdSeg(p, float2(-0.24, -0.24), float2(-0.41, -0.44), 0.070));
        d = opUnion(d, sdSeg(p, float2(-0.41, -0.44), float2(-0.05, -0.56), 0.060));
        d = opUnion(d, sdSeg(p, float2(0.24, -0.24), float2(0.41, -0.42), 0.070));
        d = opUnion(d, sdSeg(p, float2(0.41, -0.42), float2(0.11, -0.56), 0.060));

        // Cut: the hollow under the hood, the fold at the waist, the plinth courses.
        detail = sdCircle(h - float2(0.02, 0.06), 0.095);
        detail = opUnion(detail, max(abs(p.y - 0.22) - 0.022, abs(p.x) - 0.17));
        detail = opUnion(detail, max(abs(p.y - 0.79) - 0.010, abs(p.x) - 0.52));
        detail = opUnion(detail, max(abs(p.y - 0.89) - 0.010, abs(p.x) - 0.52));

        // The tear, falling from between the hands and drying where it lands.
        const float fall = frac(time * 0.28);
        core = sdCircle(p - float2(0.03, -0.44 + fall * 1.20), 0.040 * (1.0 - fall * 0.30));
        core = opUnion(core, max(abs(p.y - 0.725) - 0.011, abs(p.x - 0.03) - 0.07 * (1.0 - fall)));
    }
    else if (rite == 3)
    {
        // Flesh Loom: warp coming off a beam, a shed for the shuttle, and cloth being taken
        // up below it.
        //
        // The previous version was a rectangular frame filled edge to edge with evenly
        // spaced vertical bars, which is a cage - a symmetric grid you can see through
        // everywhere has no other reading. Two things fix it. The frame leans, so nothing
        // about it is a rectangle. And the lower half is SOLID cloth: the moment part of
        // the shape stops being see-through, the eye stops reading bars and starts reading
        // thread becoming fabric, which is the only thing a loom is for.

        // The frame leans in toward the top, braced by feet.
        d = opUnion(d, sdSeg(p, float2(-0.74, 0.90), float2(-0.54, -0.84), 0.070));
        d = opUnion(d, sdSeg(p, float2(0.74, 0.90), float2(0.54, -0.84), 0.070));
        d = opUnion(d, sdRound(p - float2(-0.74, 0.92), float2(0.19, 0.07), 0.04));
        d = opUnion(d, sdRound(p - float2(0.74, 0.92), float2(0.19, 0.07), 0.04));

        // The warp beam at the top and the cloth beam at the bottom, both rollers.
        d = opUnion(d, sdSeg(p, float2(-0.58, -0.80), float2(0.58, -0.80), 0.095));
        d = opUnion(d, sdSeg(p, float2(-0.60, 0.72), float2(0.60, 0.72), 0.110));

        // The warp: thread off the beam, running down to the fell line. Thin, and it stops
        // where the cloth starts - that boundary is the whole picture.
        const float warp = abs(frac(p.x * 4.5 + 0.5) - 0.5) / 4.5;
        d = opUnion(d, max(warp - 0.013, max(abs(p.y + 0.35) - 0.39, abs(p.x) - 0.52)));

        // The heddle frames, hung across the warp. These are what lift alternate threads,
        // so they belong above the shed, not in it.
        d = opUnion(d, max(abs(p.y + 0.30) - 0.035, abs(p.x) - 0.50));
        d = opUnion(d, max(abs(p.y + 0.18) - 0.035, abs(p.x) - 0.50));
        d = opUnion(d, sdSeg(p, float2(-0.40, -0.78), float2(-0.40, -0.32), 0.016));
        d = opUnion(d, sdSeg(p, float2(0.40, -0.78), float2(0.40, -0.32), 0.016));

        // The cloth. Solid, with a woven edge that is not quite straight, taken up over the
        // beam and hanging a little past it.
        const float selvage = 0.50 + sin(p.y * 9.0) * 0.013;
        d = opUnion(d, max(abs(p.x) - selvage, abs(p.y - 0.38) - 0.34));
        d = opUnion(d, max(abs(p.x) - 0.44, abs(p.y - 0.86) - 0.10));

        // Cut: the weft courses across the cloth, and the grain of the two beams.
        const float weft = abs(frac(p.y * 13.0 + 0.5) - 0.5) / 13.0;
        detail = max(weft - 0.011, max(abs(p.x) - 0.46, abs(p.y - 0.38) - 0.30));
        const float grain = abs(frac(p.x * 7.0 + 0.5) - 0.5) / 7.0;
        detail = opUnion(detail, max(grain - 0.012, max(abs(p.y - 0.72) - 0.07, abs(p.x) - 0.56)));

        // The shuttle, running the shed back and forth, and the fell line it is laying -
        // the line where thread stops being warp and becomes fabric.
        const float pass = sin(time * 1.1) * 0.44;
        core = sdRound(p - float2(pass, 0.03), float2(0.13, 0.045), 0.045);
        core = opUnion(core, max(abs(p.y - 0.045) - 0.010, abs(p.x) - 0.50));
    }
    else if (rite == 4)
    {
        // Ossuary Engine: a boiler on a frame, a stack, and a flywheel that actually turns.
        //
        // It was a rounded box with grooves in it, which is a crate. What makes a machine
        // read as a machine is moving parts and the linkage between them, so it has a wheel
        // now, and a rod running from the wheel into the boiler that drives with it.
        d = opUnion(d, sdBox(p - float2(0.0, 0.90), float2(0.96, 0.10)));    // bed
        d = opUnion(d, sdBox(p - float2(-0.62, 0.66), float2(0.11, 0.24)));  // legs
        d = opUnion(d, sdBox(p - float2(0.10, 0.66), float2(0.11, 0.24)));

        // The boiler: a horizontal drum, with a banded collar at each end.
        d = opUnion(d, sdRound(p - float2(-0.22, 0.24), float2(0.62, 0.34), 0.16));
        d = opUnion(d, sdRound(p - float2(-0.80, 0.24), float2(0.07, 0.38), 0.05));
        d = opUnion(d, sdRound(p - float2(0.34, 0.24), float2(0.07, 0.38), 0.05));

        // The stack, rising off the boiler with a flared cowl.
        d = opUnion(d, sdBox(p - float2(-0.52, -0.34), float2(0.13, 0.40)));
        d = opUnion(d, sdBox(p - float2(-0.52, -0.72), float2(0.20, 0.07)));

        // The flywheel. Rim, hub and six spokes, turning at a rate the eye can follow -
        // this is the single thing that stops the rite reading as a box.
        const float2 w = p - float2(0.52, 0.36);
        const float wr = length(w);
        const float wa = atan2(w.y, w.x) + time * 0.55;
        const float spokes = 6.0;
        const float sa = abs(frac(wa * spokes / 6.28318530718 + 0.5) - 0.5) * 6.28318530718 / spokes;
        float wheel = abs(wr - 0.40) - 0.055;                                // rim
        wheel = opUnion(wheel, wr - 0.10);                                   // hub
        wheel = opUnion(wheel, max(sa * wr - 0.035, wr - 0.40));             // spokes
        d = opUnion(d, wheel);

        // The connecting rod, from a crank pin on the wheel back into the boiler. It moves
        // with the wheel, so the whole assembly reads as one mechanism.
        const float2 pin = float2(0.52, 0.36) + float2(cos(wa), sin(wa)) * 0.26;
        const float2 head = float2(0.02, 0.36);
        const float2 axis = pin - head;
        const float len = length(axis);
        const float2 dir = axis / max(len, 1e-4);
        const float2 rel = p - (head + pin) * 0.5;
        d = opUnion(d, sdRound(float2(dot(rel, dir), dot(rel, float2(-dir.y, dir.x))),
                               float2(len * 0.5, 0.045), 0.04));

        // Cut: the boiler's bands, and the furnace door.
        const float band = abs(frac(p.x * 3.2 + 0.5) - 0.5) / 3.2;
        detail = max(band - 0.026, sdRound(p - float2(-0.22, 0.24), float2(0.54, 0.28), 0.12));
        detail = opUnion(detail, sdRound(p - float2(-0.58, 0.30), float2(0.19, 0.20), 0.06));

        // The furnace behind the door, breathing.
        const float breath = 0.145 + sin(time * 2.3) * 0.014;
        core = sdRound(p - float2(-0.58, 0.30), float2(0.13, 0.14), breath * 0.6);
        // And what escapes up the stack.
        core = opUnion(core, sdCircle(p - float2(-0.52, -0.78 - frac(time * 0.4) * 0.22),
                                      0.035 * (1.0 - frac(time * 0.4))));
    }
    else if (rite == 5)
    {
        // Drowned Chapel: a nave with a lit window and a bell tower, standing in water.
        //
        // The roof was upside down. The half-plane test allowed a width proportional to
        // -(y + base) instead of (y - apex), which put the wide end at the ridge and the
        // point at the eaves - a funnel hovering over a crate. It only showed up once the
        // rite was actually owned, which is why every structure now gets looked at.
        d = opUnion(d, sdBox(p - float2(-0.06, 0.44), float2(0.60, 0.50)));   // nave

        // The roof: wide at the eaves (y = -0.06), meeting at a ridge (y = -0.80).
        d = opUnion(d, max(max(abs(p.x + 0.06) * 1.22 - (p.y + 0.80), p.y + 0.06), -p.y - 0.82));

        // A bell tower off one shoulder, so the chapel is not a symmetric shed.
        d = opUnion(d, sdBox(p - float2(0.66, 0.28), float2(0.20, 0.66)));
        d = opUnion(d, max(abs(p.x - 0.66) * 1.05 - (p.y + 0.62), p.y + 0.38));
        d = opUnion(d, sdBox(p - float2(0.66, -0.76), float2(0.04, 0.14)));   // cross
        d = opUnion(d, sdBox(p - float2(0.66, -0.82), float2(0.11, 0.04)));
        detail = sdArch(p - float2(0.66, 0.14), float2(0.10, 0.16), 0.10);    // belfry opening

        // The window: the one lit thing, an arch cut into the nave.
        detail = opUnion(detail, sdArch(p - float2(-0.06, 0.38), float2(0.24, 0.32), 0.24));
        core = sdArch(p - float2(-0.06, 0.40), float2(0.17, 0.26), 0.17);

        // Courses of stone across the nave.
        const float course = abs(frac(p.y * 5.0 + 0.5) - 0.5) / 5.0;
        detail = opUnion(detail, max(course - 0.014, sdBox(p - float2(-0.06, 0.44), float2(0.54, 0.44))));

        // The waterline, rocking. What is under it is in the dark, and the reflection of
        // the window lies on it.
        const float water = 0.70 + sin(time * 0.7 + p.x * 2.4) * 0.022;
        detail = opUnion(detail, max(abs(p.y - water) - 0.022, abs(p.x) - 1.0));
        core = opUnion(core, max(abs(p.y - water - 0.06) - 0.030,
                                 abs(p.x + 0.06) - 0.13 - sin(time * 1.9) * 0.02));
    }
    else if (rite == 6)
    {
        // Pale Shepherd: a figure holding a crook, with its flock at its feet.
        //
        // It was a cone with a ball on it and a rod floating alongside, which read as a
        // lollipop next to a stick. A crook has to be HELD - the hand has to be on it, or
        // it is just another object standing in the same place.
        const float waist = 0.155 + smoothstep(0.10, 0.78, p.y) * 0.20
                          + sin(p.y * 7.0) * 0.012;
        d = opUnion(d, max(abs(p.x + 0.18) - waist, abs(p.y - 0.30) - 0.56));
        d = opUnion(d, sdRound(p - float2(-0.18, -0.28), float2(0.22, 0.09), 0.09));  // shoulders
        d = opUnion(d, sdCircle(p - float2(-0.18, -0.52), 0.19));                     // head
        // A broad hat, which is what tells a shepherd from any other robed figure. The brim
        // is wide and thin and the crown is a cone above it - a short cylinder on a short
        // disc came out as a nut on a bolt.
        d = opUnion(d, sdRound(p - float2(-0.18, -0.655), float2(0.40, 0.028), 0.028));
        d = opUnion(d, max(abs(p.x + 0.18) * 1.55 - (p.y + 0.94), p.y + 0.63));

        // The staff, and the arm that holds it. The hand closes on the shaft.
        d = opUnion(d, sdSeg(p, float2(0.30, -0.86), float2(0.34, 0.90), 0.042));
        d = opUnion(d, abs(sdCircle(p - float2(0.22, -0.84), 0.155)) - 0.042);        // the crook
        d = opUnion(d, sdSeg(p, float2(-0.02, -0.24), float2(0.28, -0.10), 0.062));   // arm
        d = opUnion(d, sdCircle(p - float2(0.31, -0.09), 0.078));                     // hand on it

        // The flock: three of them, bodies and heads, crowded at the shepherd's feet.
        for (int f = 0; f < 3; ++f)
        {
            const float fx = -0.74 + float(f) * 0.16;
            const float fy = 0.80 + float((f + 1) % 2) * 0.07;
            d = opUnion(d, sdRound(p - float2(fx, fy), float2(0.115, 0.085), 0.075));
            d = opUnion(d, sdCircle(p - float2(fx + 0.11, fy - 0.05), 0.055));
            d = opUnion(d, sdSeg(p, float2(fx - 0.04, fy + 0.07), float2(fx - 0.04, fy + 0.14), 0.020));
            d = opUnion(d, sdSeg(p, float2(fx + 0.05, fy + 0.07), float2(fx + 0.05, fy + 0.14), 0.020));
        }

        detail = sdCircle(p - float2(-0.18, -0.50), 0.135);          // the face, in the hat's shade
        detail = opUnion(detail, max(abs(p.y - 0.28) - 0.020, abs(p.x + 0.18) - 0.15));
        core = sdCircle(p - float2(-0.25, -0.52), 0.048);            // the eye under the brim
    }
    else
    {
        // Hollow Mouth: a ring of teeth around a throat that goes nowhere.
        //
        // The teeth were thin slivers of constant width, which came out as the graduations
        // on a clock face - the whole rite read as a porthole. A tooth has to TAPER: fat
        // where it meets the gum and sharp where it points, which is the only thing that
        // separates a mouth from a dial.
        const float r = length(p);
        const float a = atan2(p.y, p.x);

        // The lips: a heavy ring, slightly out of round so it is flesh and not machined.
        const float lip = 0.80 + sin(a * 3.0 + 0.7) * 0.030;
        d = abs(r - lip) - 0.135;

        // The teeth. Angular distance to the nearest of sixteen, times the radius, gives
        // the distance to the tooth's centre line in the plane; allowing a half-width that
        // falls to zero at the inner end makes each one a triangle pointing inward.
        const float count = 16.0;
        const float sa = abs(frac(a * count / 6.28318530718 + 0.5) - 0.5) * 6.28318530718 / count;
        const float outer = lip - 0.10;
        const float inner = 0.24;
        const float taper = saturate((r - inner) / (outer - inner));
        d = opUnion(d, max(sa * r - 0.105 * taper * taper, max(r - outer, inner - r)));

        // The gums behind them, so the teeth are set into something.
        d = opUnion(d, abs(r - (outer - 0.02)) - 0.075);

        // The throat: genuinely dark, and deepening inward, so the middle of the rite is a
        // hole rather than a flat disc with a ring drawn round it.
        detail = sdCircle(p, inner + 0.02);
        detail = opUnion(detail, abs(r - lip) - 0.020);

        // What is down there, breathing.
        core = sdCircle(p, (0.085 + sin(time * 1.7) * 0.014));
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
    // light is. The gradient of the distance field is the surface normal, so the shape
    // shades itself without anyone painting a highlight into it by hand.
    //
    // Sampled at a fixed offset rather than taken from ddx/ddy. A screen derivative of a
    // signed distance field flips sign across the shape's medial axis, which put a hard
    // seam straight down the middle of every symmetric rite - the light was correct on
    // both halves, but the change between them happened in one pixel.
    const float eps = 0.012;
    float cx0, cx1, dx0, dx1;
    const float gx = Structure(rite, q + float2(eps, 0.0), time, cx0, dx0)
                   - Structure(rite, q - float2(eps, 0.0), time, cx1, dx1);
    const float gy = Structure(rite, q + float2(0.0, eps), time, cx0, dx0)
                   - Structure(rite, q - float2(0.0, eps), time, cx1, dx1);
    const float2 grad = normalize(float2(gx, gy) + 1e-6);
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

    // Deliberately NOT multiplied by input.color.a. Gating on the element's alpha forced the
    // element to carry an opaque fill for the material to draw at all, which meant any frame
    // the material did not draw - the creation frame, a hot reload, a dropped pipeline - showed
    // that fill as a solid slab across the rite's rect. Ignoring it lets the element be fully
    // transparent, so a missed frame is invisible instead of a black square.
    return float4(outCol, outA);
}
