// Post-process pass: FXAA (Fast Approximate Anti-Aliasing).
//
// Applied after tonemapping on the LDR R8G8B8A8 buffer so edge detection
// works on perceptual luminance rather than linear HDR values.
//
// Algorithm: FXAA quality variant using only integer Load() calls so no
// sampler is required.  The neighbourhood samples are fetched with
// SV_Position pixel coordinates, the dominant edge direction is computed
// from luma gradients, the edge endpoints are found by walking up to
// ±8 pixels in each direction, and the final blend samples the perpendicular
// neighbour pixel (lerped against centre).  This is equivalent in quality to
// the bilinear-filter version because the final blend is always between the
// centre pixel and exactly one adjacent pixel in the perpendicular direction.
//
// Binding layout (identical to tonemap.slang — no sampler needed):
//   set 0, binding 0 — bindless combined image samplers (Load() ignores sampler)

[[vk::binding(0, 0)]]
Texture2D g_textures[];

// Push constant: bindless slot index of the LDR source image.
struct FxaaPush { uint ldrSlot; };
[[vk::push_constant]] FxaaPush pc;

// ── Helpers ────────────────────────────────────────────────────────────────

float3 LoadRGB(Texture2D tex, int2 coord)
{
    return tex.Load(int3(coord, 0)).rgb;
}

// Rec. 709 perceptual luminance.
float Luma(float3 rgb)
{
    return dot(rgb, float3(0.2126f, 0.7152f, 0.0722f));
}

// ── Vertex shader ──────────────────────────────────────────────────────────
[shader("vertex")]
float4 vertexMain(uint vertexID : SV_VertexID) : SV_Position
{
    float2 uv = float2((vertexID << 1u) & 2u, vertexID & 2u);
    return float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
}

// ── Fragment shader ────────────────────────────────────────────────────────
[shader("fragment")]
float4 fragmentMain(float4 svPos : SV_Position) : SV_Target0
{
    const int2 coord = int2(svPos.xy);
    Texture2D  tex   = g_textures[pc.ldrSlot];

    // ── 5-tap cross ────────────────────────────────────────────────────────
    const float3 rgbM  = LoadRGB(tex, coord);
    const float3 rgbN  = LoadRGB(tex, coord + int2( 0, -1));
    const float3 rgbS  = LoadRGB(tex, coord + int2( 0,  1));
    const float3 rgbW  = LoadRGB(tex, coord + int2(-1,  0));
    const float3 rgbE  = LoadRGB(tex, coord + int2( 1,  0));

    const float lumaM = Luma(rgbM);
    const float lumaN = Luma(rgbN);
    const float lumaS = Luma(rgbS);
    const float lumaW = Luma(rgbW);
    const float lumaE = Luma(rgbE);

    const float lumaMin   = min(lumaM, min(min(lumaN, lumaS), min(lumaW, lumaE)));
    const float lumaMax   = max(lumaM, max(max(lumaN, lumaS), max(lumaW, lumaE)));
    const float lumaRange = lumaMax - lumaMin;

    // FXAA quality thresholds (preset 12):
    //   skip very dark pixels (< 0.0833) and low-contrast regions (< 12.5% of peak)
    static const float kAbsoluteThreshold = 0.0833f;
    static const float kRelativeThreshold = 0.125f;

    if (lumaRange < max(kAbsoluteThreshold, lumaMax * kRelativeThreshold))
    {
        return float4(rgbM, 1.0f);
    }

    // ── 4 diagonal corners (for axis-aligned gradient + box filter) ────────
    const float3 rgbNW = LoadRGB(tex, coord + int2(-1, -1));
    const float3 rgbNE = LoadRGB(tex, coord + int2( 1, -1));
    const float3 rgbSW = LoadRGB(tex, coord + int2(-1,  1));
    const float3 rgbSE = LoadRGB(tex, coord + int2( 1,  1));

    const float lumaNW = Luma(rgbNW);
    const float lumaNE = Luma(rgbNE);
    const float lumaSW = Luma(rgbSW);
    const float lumaSE = Luma(rgbSE);

    // ── Edge direction ─────────────────────────────────────────────────────
    const float edgeH = abs(-2.0f * lumaW + lumaNW + lumaSW)
                      + abs(-2.0f * lumaM + lumaN  + lumaS ) * 2.0f
                      + abs(-2.0f * lumaE + lumaNE + lumaSE);
    const float edgeV = abs(-2.0f * lumaN + lumaNW + lumaNE)
                      + abs(-2.0f * lumaM + lumaW  + lumaE ) * 2.0f
                      + abs(-2.0f * lumaS + lumaSW + lumaSE);

    const bool isHorizontal = (edgeH >= edgeV);

    // Perpendicular neighbour pixels and the blend step direction.
    const float luma1     = isHorizontal ? lumaS : lumaE;
    const float luma2     = isHorizontal ? lumaN : lumaW;
    const float gradient1 = abs(luma1 - lumaM);
    const float gradient2 = abs(luma2 - lumaM);
    const int   stepSign  = (gradient1 >= gradient2) ? 1 : -1;

    // Blend towards the neighbour with the larger gradient.
    const int2 stepDirPx  = isHorizontal ? int2(0, stepSign) : int2(stepSign, 0);
    const int2 edgeDirPx  = isHorizontal ? int2(1, 0)        : int2(0, 1);

    // ── Edge endpoint search (±8 steps) ───────────────────────────────────
    const float lumaAvg       = (luma1 + luma2) * 0.5f;
    const float lumaThreshold = abs(lumaAvg - lumaM);

    int2 pos1 = coord + edgeDirPx;
    int2 pos2 = coord - edgeDirPx;
    bool reached1 = false;
    bool reached2 = false;

    [unroll(8)]
    for (int i = 0; i < 8; ++i)
    {
        if (!reached1)
        {
            reached1 = abs(Luma(LoadRGB(tex, pos1)) - lumaAvg) >= lumaThreshold;
            if (!reached1) pos1 += edgeDirPx;
        }
        if (!reached2)
        {
            reached2 = abs(Luma(LoadRGB(tex, pos2)) - lumaAvg) >= lumaThreshold;
            if (!reached2) pos2 -= edgeDirPx;
        }
    }

    const float dist1  = isHorizontal ? abs(pos1.x - coord.x) : abs(pos1.y - coord.y);
    const float dist2  = isHorizontal ? abs(pos2.x - coord.x) : abs(pos2.y - coord.y);
    const float distMin = min(dist1, dist2);
    const float edgeLen = dist1 + dist2;

    // How far along the edge are we?  0 at centre, 0.5 at endpoint → blend 0..1.
    float blendFactor = max(0.0f, 0.5f - distMin / edgeLen);

    // ── Sub-pixel blend (3×3 box filter) ──────────────────────────────────
    const float3 rgbBox  = (rgbN + rgbS + rgbW + rgbE
                          + rgbNW + rgbNE + rgbSW + rgbSE) * (1.0f / 8.0f);
    const float  lumaBox = Luma(rgbBox);
    const float  subBlend   = clamp(abs(lumaBox - lumaM) / lumaRange, 0.0f, 1.0f);
    const float  subBlendSq = subBlend * subBlend * 0.75f;
    blendFactor = max(blendFactor, subBlendSq);

    // ── Final blend between centre and perpendicular neighbour ─────────────
    // blendFactor is in [0, 0.5], scale to [0, 1] for the lerp.
    const float3 neighbour = LoadRGB(tex, coord + stepDirPx);
    const float3 result    = lerp(rgbM, neighbour, min(blendFactor * 2.0f, 1.0f));

    return float4(result, 1.0f);
}
