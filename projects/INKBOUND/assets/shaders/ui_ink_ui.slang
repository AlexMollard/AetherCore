// Inky pixel-art UI material (UIMaterial, "ui_ink_ui"). Applied to a widget so its own shapes
// (slider track/fill/handle, toggle track/knob) render like blocky brushed ink instead of clean
// vector: the shape is pixel-snapped, its edge is roughened with noise, and a faint grain breaks up
// the fill. Reuses the shared ui_shapes vertex, so it receives the real shape geometry + colour.
//   params.x = time (slow grain drift);  color1 = optional ink-edge tint (0 = darken the base).

[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

struct ShapesPush
{
    float4   screenSize;
    uint64_t commandData; // unused here; layout parity with the vertex
    uint     pad0;
    uint     pad1;
    float4   params;      // x = time
    float4   color0;
    float4   color1;      // ink-edge tint (0,0,0,0 -> darken the shape's own colour)
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

static const uint kShapeRect   = 0;
static const uint kShapeCircle = 1;

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
    const float2 sizePx = max(input.localScale, float2(1.0, 1.0));

    // Pixel-snap local space to ~3px cells so the shape reads as chunky ink.
    const float2 cell = 3.0 / sizePx * 2.0; // cell size in [-1,1] local units
    const float2 lp = (floor(input.local / cell) + 0.5) * cell;

    // Edge roughness (in px) from noise along the shape, so borders look brushed, not machined.
    const float2 npos = (lp * 0.5 + 0.5) * sizePx;
    const float edgeNoise = (vnoise(npos * 0.25) - 0.5) * 2.0; // -1..1
    const float rough = edgeNoise * 1.6; // px

    float inside; // >0 inside
    if (input.type == kShapeCircle)
    {
        const float2 halfSz = max(sizePx * 0.5, float2(0.001, 0.001));
        const float2 pp = lp * halfSz;                       // px from centre
        const float r = min(halfSz.x, halfSz.y);
        inside = (r - length(pp)) + rough;                   // signed px, roughened
    }
    else // rect (with optional corner radius) - rounded-box SDF, matches ui_shapes
    {
        const float2 halfSz = max(sizePx * 0.5, float2(0.001, 0.001));
        const float radius = min(input.cornerRadius, min(halfSz.x, halfSz.y));
        const float2 pp = abs(lp * halfSz);
        const float2 q = pp - (halfSz - radius);
        const float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
        inside = (-d) + rough;                               // signed px, roughened
    }

    if (inside < 0.5)
    {
        discard;
    }

    // Grain: a quantised noise that breaks up the fill like ink soaked into paper; drifts slowly.
    const float grain = floor(vnoise(npos * 0.14 + time * 0.15) * 3.0) / 3.0;
    float3 rgb = input.color.rgb * (0.92 + 0.16 * grain);

    // Ink edge: a thin wet rim (tint with color1, else a slightly darkened base). Kept subtle so
    // thin shapes like slider fills stay readable rather than washing out to the rim colour.
    const float edge = 1.0 - saturate(inside / 2.0);
    const float3 edgeCol = dot(pc.color1.rgb, float3(1.0, 1.0, 1.0)) > 0.001 ? pc.color1.rgb : input.color.rgb * 0.5;
    rgb = lerp(rgb, edgeCol, edge * 0.5);

    return float4(rgb, input.color.a);
}
