// The aether meter, drawn as an INK WELL instead of a progress bar (UIMaterial, "ui_inkwell").
// A squat glass pot with a heavy rim: the ink sits in the bottom and its surface rises and falls with
// how much you have left. The surface has weight - it wobbles, it catches a bright meniscus line, and
// it goes warm and restless when the well is nearly dry.
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

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    const float time = pc.params.x;
    const float fill = saturate(pc.params.y);

    // Widget space: x,y in [-1,1]. Work in a "pot" space where 0 is the bottom of the ink.
    const float2 p = input.local;
    const float ux = p.x * 0.5 + 0.5;          // 0..1 across
    const float uy = 1.0 - (p.y * 0.5 + 0.5);  // 0 at the bottom, 1 at the top

    // --- the vessel: a rounded pot, narrower at the neck ------------------------------------
    // Waist profile: full width low down, pinched near the top so it reads as a well, not a box.
    const float neck = smoothstep(0.62, 1.0, uy);
    const float halfW = lerp(0.94, 0.66, neck);
    const float inside = step(abs(p.x), halfW);
    if (inside < 0.5) { discard; }

    const float edge = smoothstep(halfW, halfW - 0.10, abs(p.x)); // 0 at the glass wall, 1 inside

    // --- the ink surface ---------------------------------------------------------------------
    // The level sits at `fill`, with a little slosh so it never looks like a static gauge. Two
    // out-of-phase waves keep it from reading as a clean sine.
    const float slosh = (sin(time * 2.1 + ux * 5.0) * 0.012 + sin(time * 1.3 - ux * 3.1) * 0.008)
                      * smoothstep(0.02, 0.25, fill);   // an almost-empty well barely moves
    const float surface = fill + slosh;
    const float below = smoothstep(surface + 0.012, surface - 0.012, uy); // 1 under the surface

    // --- colour --------------------------------------------------------------------------------
    // Cyan ink normally; it turns warm and anxious as the well runs down, which is the same warning
    // the old bar gave, just carried by the liquid itself.
    const float low = smoothstep(0.35, 0.10, fill);
    const float3 inkDeep = lerp(float3(0.05, 0.20, 0.34), float3(0.30, 0.13, 0.04), low);
    const float3 inkTop  = lerp(float3(0.36, 0.80, 1.00), float3(1.00, 0.62, 0.24), low);

    // Depth shading: darker the further under the surface, so the pot has volume.
    const float depth = saturate((surface - uy) / max(surface, 0.001));
    float3 ink = lerp(inkTop, inkDeep, depth * 0.85);

    // Grain, so the fill is brushed rather than flat.
    ink *= 0.90 + 0.10 * vnoise(float2(ux * 14.0, uy * 22.0 - time * 0.6));

    // Meniscus: a bright line right at the surface, brighter when the level is moving.
    const float meniscus = smoothstep(0.030, 0.0, abs(uy - surface));
    ink += inkTop * meniscus * 0.85;

    // --- glass and rim ---------------------------------------------------------------------------
    const float3 empty = float3(0.045, 0.060, 0.095);       // the unfilled pot
    float3 col = lerp(empty, ink, below);

    // A cold highlight down the left of the glass, and a dark rim all round.
    col += float3(0.10, 0.16, 0.22) * smoothstep(0.35, 0.0, abs(p.x + halfW * 0.62)) * 0.25;
    col *= lerp(0.45, 1.0, edge);

    // The lip of the well: a solid band across the top so it reads as an opening.
    const float lip = smoothstep(0.90, 0.97, uy);
    col = lerp(col, float3(0.16, 0.22, 0.30), lip);

    // Alpha: solid pot, with the glass wall fading a touch so it sits on the HUD rather than on top.
    const float a = lerp(0.80, 1.0, edge) * input.color.a;
    return float4(col, a);
}
