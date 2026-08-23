// The parish itself, drawn analytically ("ui_parish", UIEffect, background = true).
//
// This replaces a flat quad standing in for the ground and a 2D light map that washed it out
// whatever it was tinted. Everything here is computed: a sky that falls to a horizon, two
// ranks of distant architecture, fog lying in the low ground, and a flagged floor in
// perspective that catches what little light there is. No texture, no lighting pass, so
// nothing can lift the floor to the brightest thing on screen behind my back.
//
//   params = (time, dread 0..1, shake 0..1, wave 0..1)
//   color0 = the void (the ramp's darkest)   color1 = dread, for the souring
//
// color0's ALPHA carries where the wave started, as a fraction of the width. params is full
// and only the void's rgb is ever read, so the spare channel is used rather than adding a
// second effect - noted here and at the call site in Parish.cs so it is not a surprise.
//
// The palette is passed in rather than written here, so this cannot drift from Palette.cs.

[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

struct BgPush
{
    float4 screenSize;
    float4 rect;
    float4 params;
    float4 color0;
    float4 color1;
};

[[vk::push_constant]] BgPush pc;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

[shader("vertex")]
VSOutput vertexMain(uint vertexId : SV_VertexID)
{
    const float2 uvRect[6] = {
        float2(0.0, 1.0), float2(1.0, 1.0), float2(1.0, 0.0),
        float2(0.0, 1.0), float2(1.0, 0.0), float2(0.0, 0.0),
    };
    const float2 uv = uvRect[vertexId % 6u];
    const float2 sp = pc.rect.xy + uv * pc.rect.zw;

    VSOutput o;
    o.position = float4((sp.x / pc.screenSize.x) * 2.0 - 1.0, (sp.y / pc.screenSize.y) * 2.0 - 1.0, 0.0, 1.0);
    o.uv = uv;
    return o;
}

float hash11(float x) { return frac(sin(x * 12.9898) * 43758.5453); }

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
    for (int i = 0; i < 4; ++i) { s += a * vnoise(p); p *= 2.03; a *= 0.5; }
    return s;
}

// The one ramp, mirrored from scripts/Palette.cs. Written as a function of a single 0..1
// value so everything in here samples the SAME eight steps the rest of the game is built
// from, rather than inventing greys of its own.
float3 Ramp(float t)
{
    const float3 ink   = float3(0.027, 0.031, 0.043);
    const float3 slate = float3(0.082, 0.094, 0.122);
    const float3 stone = float3(0.137, 0.157, 0.200);
    const float3 ash   = float3(0.227, 0.255, 0.314);
    const float3 bone  = float3(0.431, 0.463, 0.525);
    t = saturate(t);
    if (t < 0.25) { return lerp(ink,   slate, t / 0.25); }
    if (t < 0.50) { return lerp(slate, stone, (t - 0.25) / 0.25); }
    if (t < 0.75) { return lerp(stone, ash,   (t - 0.50) / 0.25); }
    return lerp(ash, bone, (t - 0.75) / 0.25);
}

// A skyline of blocks and spires at a given depth. Deterministic per rank, so the parish is
// the same parish every frame - it is a place, not a noise field.
float Skyline(float x, float rank, float scale, float baseHeight)
{
    const float cell = floor(x * scale + rank * 17.0);
    const float r = hash11(cell + rank * 91.7);
    float h = baseHeight * (0.45 + r * 0.55);
    // A minority of them reach - spires, towers, the things you can see from the parish.
    if (hash11(cell * 1.7 + rank) > 0.86) { h *= 1.9; }
    return h;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    const float time = pc.params.x;
    const float dread = saturate(pc.params.y);
    const float shake = saturate(pc.params.z);

    const float aspect = pc.rect.z / max(pc.rect.w, 1.0);
    float2 uv = input.uv;
    // The whole scene shifts when something arrives, rather than the camera being moved -
    // one place to shake, and the UI over the top stays put where it is readable.
    uv.x += sin(time * 41.0) * 0.004 * shake;
    uv.y += cos(time * 37.0) * 0.004 * shake;

    // The horizon sits low: this is a parish seen from inside it, not a landscape.
    const float horizon = 0.62;
    float3 col;

    if (uv.y < horizon)
    {
        // ── Above the horizon ────────────────────────────────────────────────────────
        // Falls from near-black overhead to a faint pallor at the skyline, which is the
        // only thing suggesting there is a world beyond the parish.
        const float h = uv.y / horizon;
        col = Ramp(0.04 + pow(h, 2.0) * 0.52);

        // Two ranks of architecture, the far one paler and flatter, so distance reads as
        // atmosphere rather than as scale alone.
        const float x = uv.x * aspect;
        const float far = Skyline(x, 1.0, 7.0, 0.16);
        const float near = Skyline(x, 2.0, 3.5, 0.26);
        const float dFar = horizon - far;
        const float dNear = horizon - near;
        if (uv.y > dFar)  { col = lerp(col, Ramp(0.26), 0.80); }
        if (uv.y > dNear) { col = lerp(col, Ramp(0.14), 0.92); }

        // Fog lying against the skyline, thickest right at the horizon line.
        const float fog = fbm(float2(x * 2.2 + time * 0.012, uv.y * 3.0 - time * 0.008));
        const float band = smoothstep(horizon - 0.22, horizon, uv.y);
        col = lerp(col, Ramp(0.30 + fog * 0.16), band * (0.55 + fog * 0.35));
    }
    else
    {
        // ── The floor ────────────────────────────────────────────────────────────────
        // A proper ground-plane projection. For an eye above a plane, the distance to what
        // you see at a given height below the horizon is inversely proportional to that
        // height - so rows crowd toward the horizon on their own, no fudging.
        const float d = max(uv.y - horizon, 0.0005);
        const float dist = 0.55 / d;                       // distance out across the floor
        const float lateral = (uv.x - 0.5) * aspect * dist;

        // Flagstone grid. The tiling rate has to be high enough that the near floor is MANY
        // stones across - at the old rate less than one course covered the whole lower half,
        // which is exactly why it read as a low-resolution blur rather than as a surface.
        const float tile = 4.5;
        const float rowY = dist * tile;
        const float colX = lateral * tile;

        // Every other course offset half a stone, so they break like laid flagging.
        const float course = floor(rowY);
        const float offset = (fmod(abs(course), 2.0) < 1.0) ? 0.0 : 0.5;
        const float2 cell = float2(floor(colX + offset), course);
        const float2 g = float2(frac(colX + offset), frac(rowY));

        // Mortar, measured in SCREEN pixels via the derivative rather than in cell space.
        // This is what keeps the joints one hairline wide at every depth instead of swelling
        // into black bands up close and aliasing into moire at the back.
        const float2 w = fwidth(float2(colX, rowY)) * 1.5 + 1e-5;
        const float2 edge = min(g, 1.0 - g);
        const float joint = min(smoothstep(0.0, w.x, edge.x), smoothstep(0.0, w.y, edge.y));

        // Per-stone wear, so no two flags are quite the same tone.
        const float wear = hash21(cell * 3.1);
        float stone = 0.16 + wear * 0.13;
        stone *= 0.30 + 0.70 * joint;

        // Light falls off with distance: the near ground is the readable part and the far
        // ground sinks into the fog the skyline stands in.
        const float fall = saturate(1.0 - dist * 0.045);
        col = Ramp(stone * (0.25 + fall * 0.75) + 0.015);

        // A wet sheen down the centre, picking out the near stones. Most of what stops a
        // floor looking like a printed texture is the light moving across it.
        const float sheen = pow(saturate(1.0 - abs(uv.x - 0.5) * 1.7), 3.0) * saturate(d * 5.0);
        col += Ramp(0.60) * sheen * 0.07;

        // Ground fog, thickest where the floor meets the horizon and gone by mid-ground.
        const float mist = fbm(float2(uv.x * 5.0 - time * 0.03, uv.y * 14.0)) * saturate(1.0 - d * 9.0);
        col = lerp(col, Ramp(0.22), mist * 0.60);
    }

    // ── The wave ─────────────────────────────────────────────────────────────────────
    // A front of light rolling out across the floor toward the viewer, fired by every
    // purchase. It accelerates as it comes, and it is brightest in line with whatever was
    // bought, so spending reads as something leaving that thing and reaching you.
    const float wave = saturate(pc.params.w);
    if (wave > 0.0)
    {
        const float front = horizon + (1.0 - horizon) * wave * wave;
        const float width = 0.028 + wave * 0.075;
        const float band = smoothstep(width, 0.0, abs(uv.y - front)) * step(horizon, uv.y);
        const float lane = 0.35 + 0.65 * smoothstep(0.80, 0.0, abs(uv.x - pc.color0.a));
        col += Ramp(0.92) * band * lane * (1.0 - wave * 0.7) * 1.35;
        // A wash of light ahead of the front, so the floor it is about to reach lifts too -
        // a single bright line reads as a scanline, a line with light in front of it reads
        // as something travelling.
        col += Ramp(0.55) * smoothstep(front + 0.16, front, uv.y) * step(horizon, uv.y)
             * lane * (1.0 - wave) * 0.22;
        // And a breath of light on the skyline as it sets off, so the whole parish
        // registers the purchase and not just the strip of floor the front is on.
        col += Ramp(0.45) * smoothstep(0.0, 0.18, wave) * (1.0 - wave) * 0.11;
    }

    // The horizon itself: a thin catch of light, which is what makes the two halves read as
    // a ground plane meeting a sky rather than as two fills.
    const float lip = smoothstep(0.014, 0.0, abs(uv.y - horizon))
                    * (0.35 + 0.65 * fbm(float2(uv.x * 6.0, 3.0)))   // uneven, not a ruled line
                    * smoothstep(0.0, 0.28, uv.x) * smoothstep(1.0, 0.72, uv.x);
    col += Ramp(0.78) * lip * 0.45;

    // Dread sours the whole place toward rust - the one hue allowed in here, and only as
    // far as the meter has actually filled.
    const float2 e2 = uv - 0.5;
    const float creep = saturate(dot(e2, e2) * 2.6);
    col = lerp(col, pc.color1.rgb * 0.30, dread * (0.05 + creep * 0.30));

    // Vignette, then grain so the large dark areas do not band on an 8-bit display.
    const float2 c = uv - 0.5;
    col *= 1.0 - saturate(dot(c, c) * 0.70);
    col += (hash21(uv * pc.screenSize.xy) - 0.5) * 0.010;

    return float4(max(col, pc.color0.rgb), 1.0);
}
