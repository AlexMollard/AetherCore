// The aether meter, drawn as an INK WELL (UIMaterial, "ui_inkwell").
//
// Not a bottle of blue liquid - a squat, hand-blown well of something black and awake. The ink is
// near-black like the ink you actually draw with; all its light lives in the skin on top, a cold
// meniscus with a sick glow under it. It creeps: strands cling to the inside of the glass above the
// surface and slide back down, the surface breathes rather than sloshes, and grit turns over in the
// depths. As the well runs dry the ink sours to a dark rust and gets agitated.
//   params.x = time,  params.y = fill (0..1)
// Reuses the shared ui_shapes vertex, so it receives the real widget geometry.

[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

struct ShapesPush
{
    float4   screenSize;
    uint64_t commandData;
    uint     pad0;
    uint     pad1;
    float4   params;      // x = time, y = fill
    float4   color0;
    float4   color1;
};

[[vk::push_constant]] ShapesPush pc;

// MUST match ui_shapes.slang VSOutput exactly.
struct VSOutput
{
    float4 position      : SV_Position;
    float4 color         : TEXCOORD0;
    float2 local         : TEXCOORD1; // [-1,1]
    float2 localScale    : TEXCOORD2; // rect px size
    float  cornerRadius  : TEXCOORD3;
    nointerpolation uint type        : TEXCOORD4;
    float2 texUV         : TEXCOORD5;
    nointerpolation uint textureSlot : TEXCOORD6;
    nointerpolation uint flags       : TEXCOORD7;
};

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

float fbm(float2 p)
{
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 3; ++i) { s += a * vnoise(p); p *= 2.07; a *= 0.5; }
    return s;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    const float time = pc.params.x;
    const float fill = saturate(pc.params.y);

    // Snap every sample to a coarse ART GRID before anything is computed with it, exactly as
    // ink_field.slang does for the ink in the world. Everything downstream then evaluates once per
    // cell instead of once per screen pixel, so the silhouette, the meniscus, the strands and the
    // glass all come out as chunky uniform blocks. Without this the well was a smooth high-resolution
    // render sitting in a game made of 16-pixel tiles - the one object on screen that was not pixel
    // art, which is exactly how it read.
    const float2 kCells = float2(20.0, 26.0); // art pixels across the widget
    const float2 raw = input.local;
    const float2 p = (floor((raw * 0.5 + 0.5) * kCells) + 0.5) / kCells * 2.0 - 1.0;
    const float ux = p.x * 0.5 + 0.5;          // 0..1 across
    const float uy = 1.0 - (p.y * 0.5 + 0.5);  // 0 at the bottom, 1 at the top

    // --- silhouette: bulbous belly, short choked neck, heavy uneven lip ------------------------
    // Hand-made, not moulded: the profile is a curve and the edge carries a little noise so the
    // glass looks old and blown rather than machined.
    // Wide rounded belly, shoulders drawing in to a choked neck, and a lip that stays NARROWER than
    // the body - an inkwell squats, it does not stand like a bottle.
    float halfW = 0.95;
    halfW -= 0.40 * smoothstep(0.52, 0.88, uy);                        // shoulders pull in
    halfW += 0.09 * smoothstep(0.90, 1.0, uy);                         // a small lip, not a tabletop
    halfW *= 1.0 - 0.55 * pow(saturate(1.0 - uy / 0.13), 2.0);         // round the base off the floor
    // No edge wobble any more: the art grid already gives the silhouette its handmade steps, and
    // noise on top of a 20-cell profile just reads as a ragged, broken outline.
    if (abs(p.x) > halfW) { discard; }

    const float inner = smoothstep(halfW, halfW - 0.13, abs(p.x));     // 0 at the glass, 1 inside

    // --- the surface: thick, so it heaves rather than ripples ----------------------------------
    const float heave = (sin(time * 0.85 + ux * 3.1) * 0.010 + sin(time * 0.53 - ux * 1.9) * 0.007)
                      * smoothstep(0.02, 0.30, fill);
    const float surface = lerp(0.10, 0.82, fill) + heave;
    const float below = smoothstep(surface + 0.010, surface - 0.010, uy);

    // --- the ink -------------------------------------------------------------------------------
    // Near-black, the same ink you draw with. All the colour is light sitting ON it, never in it.
    const float sour = smoothstep(0.35, 0.08, fill);                   // running dry
    const float3 glowCol = lerp(float3(0.22, 0.72, 0.95), float3(0.95, 0.30, 0.12), sour);

    const float depth = saturate((surface - uy) / max(surface, 0.001));
    float3 ink = lerp(float3(0.060, 0.098, 0.130), float3(0.014, 0.020, 0.030), depth); // black body

    // Something turning over down there: slow sediment, and a faint bloom of light from under the
    // skin that never quite resolves into anything.
    const float churn = fbm(float2(ux * 3.4, uy * 5.0 - time * 0.16));
    ink += glowCol * 0.055 * churn * (1.0 - depth * 0.6);
    ink *= 0.86 + 0.14 * fbm(float2(ux * 9.0, uy * 13.0 + time * 0.07));

    // The skin: a bright cold meniscus, with the glow bleeding a little way down into the ink.
    const float skin = smoothstep(0.028, 0.0, abs(uy - surface));
    const float underGlow = smoothstep(0.16, 0.0, max(0.0, surface - uy));
    ink += glowCol * (skin * 1.15 + underGlow * 0.22);

    // --- creeping strands above the surface ------------------------------------------------------
    // Ink climbing the inside of the glass and sliding back. This is the bit that makes it read as
    // alive rather than as a drink.
    const float above = max(0.0, uy - surface);
    const float lane = vnoise(float2(floor(ux * 7.0), 11.3));                   // a few fixed lanes
    const float crawl = frac(lane * 3.1 + time * 0.055);                        // each creeps at its own rate
    const float reach = (0.10 + 0.22 * lane) * (0.35 + 0.65 * crawl);
    const float strand = smoothstep(reach, 0.0, above)
                       * smoothstep(0.30, 0.72, vnoise(float2(ux * 16.0, uy * 4.0 - time * 0.20)))
                       * (1.0 - inner * 0.35);                                  // hugs the glass
    const float3 empty = float3(0.030, 0.038, 0.055);
    float3 col = lerp(empty, ink, below);
    col = lerp(col, lerp(float3(0.020, 0.030, 0.042), glowCol * 0.55, 0.25), strand * (1.0 - below));

    // --- the glass ------------------------------------------------------------------------------
    // Dim, smeared, faintly lit from the ink below rather than from any room light.
    col += glowCol * 0.10 * smoothstep(0.55, 0.0, abs(p.x + halfW * 0.55)) * below;
    col *= lerp(0.40, 1.0, inner);

    const float wall = smoothstep(halfW - 0.15, halfW - 0.02, abs(p.x));
    col = lerp(col, float3(0.180, 0.215, 0.260), wall * 0.95);                  // dull glass, but a readable rim

    // Heavy lip, and a cold catch-light on its rim.
    const float lip = smoothstep(0.90, 0.96, uy);
    col = lerp(col, float3(0.085, 0.105, 0.135), lip);
    const float lipEdge = smoothstep(0.975, 1.0, uy);
    col = lerp(col, float3(0.26, 0.34, 0.44), lipEdge);

    // The mouth swallows light - a dark throat under the lip so it reads as an opening, not a cap.
    const float throat = smoothstep(0.86, 0.94, uy) * smoothstep(0.55, 0.95, 1.0 - abs(p.x) / halfW);
    col *= lerp(1.0, 0.30, throat * (1.0 - below));

    // Sit it on a shadow.
    col *= lerp(0.45, 1.0, smoothstep(0.0, 0.09, uy));

    // Posterise to a small palette. Pixel art is a grid AND a limited set of colours; keeping the
    // full gradient would leave soft banding inside otherwise crisp blocks.
    col = floor(col * 16.0 + 0.5) / 16.0;

    return float4(col, input.color.a);
}
