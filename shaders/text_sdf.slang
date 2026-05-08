// SDF text rendering shader.
//
// Vertices are generated procedurally from SV_VertexID (6 per glyph quad).
// Glyph data is read from a device-addressable buffer; the host issues one
// draw call for all visible glyphs in a pass.
//
// Atlas format: R8_UNORM single-channel SDF where:
//   0.0 = far outside glyph, 0.5 = glyph edge, 1.0 = far inside glyph.
//
// Anti-aliasing: screen-space derivative (fwidth) of the SDF value is used
// to set a per-pixel smooth transition width so edges stay crisp at any size.

// Bindless texture array - set 0, binding 0.
[[vk::binding(0, 0)]] Texture2D g_textures[];

// Immutable linear sampler - set 0, binding 1.
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

struct GlyphInstance
{
    float4 glyphRect;
    float4 uvRect;
    float4 color;
};

struct TextBatchPush
{
    float4 screenSize;   // .xy = viewport dimensions, .zw unused
    uint   atlasSlot;
    uint   _pad0;
    uint64_t glyphDataAddr;
};

[[vk::push_constant]] TextBatchPush pc;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float4 color    : TEXCOORD1;
};

[shader("vertex")]
VSOutput vertexMain(uint vertexId : SV_VertexID)
{
    const uint glyphIndex = vertexId / 6u;
    const uint corner = vertexId % 6u;

    GlyphInstance* glyphs = (GlyphInstance*)pc.glyphDataAddr;
    const GlyphInstance glyph = glyphs[glyphIndex];

    // Glyph quad corners in screen pixels, top-left origin.
    // Two CCW triangles: (0,1,2) and (0,2,3).
    //   3 --- 2
    //   |   / |
    //   |  /  |
    //   | /   |
    //   0 --- 1
    const float x0 = glyph.glyphRect.x;
    const float y0 = glyph.glyphRect.y;
    const float x1 = glyph.glyphRect.x + glyph.glyphRect.z;
    const float y1 = glyph.glyphRect.y + glyph.glyphRect.w;

    const float2 positions[6] = {
        float2(x0, y1),   // 0 bottom-left
        float2(x1, y1),   // 1 bottom-right
        float2(x1, y0),   // 2 top-right
        float2(x0, y1),   // 3 bottom-left  (second triangle)
        float2(x1, y0),   // 4 top-right
        float2(x0, y0),   // 5 top-left
    };

    const float u0 = glyph.uvRect.x;
    const float v0 = glyph.uvRect.y;
    const float u1 = glyph.uvRect.z;
    const float v1 = glyph.uvRect.w;

    const float2 uvs[6] = {
        float2(u0, v1),
        float2(u1, v1),
        float2(u1, v0),
        float2(u0, v1),
        float2(u1, v0),
        float2(u0, v0),
    };

    const float2 sp = positions[corner];

    // Convert screen-pixel coords to Vulkan NDC: x in [-1,+1], y in [-1,+1]
    // with (0,0) at top-left in pixel space and (-1,-1) at top-left in NDC.
    const float ndcX = (sp.x / pc.screenSize.x) * 2.0f - 1.0f;
    const float ndcY = (sp.y / pc.screenSize.y) * 2.0f - 1.0f;

    VSOutput output;
    output.position = float4(ndcX, ndcY, 0.0, 1.0);
    output.uv       = uvs[corner];
    output.color    = glyph.color;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    // Sample the single-channel R8_UNORM SDF atlas.
    const float dist = g_textures[pc.atlasSlot].Sample(g_linearSampler, input.uv).r;

    // Screen-space derivative gives the transition width in SDF value space.
    // Multiplying by 0.7 is a common empirical tuning factor.
    const float w = fwidth(dist) * 0.7;

    // Edge is at dist == 0.5.  smoothstep gives smooth anti-aliased alpha.
    const float alpha = smoothstep(0.5 - w, 0.5 + w, dist);

    if (alpha < 0.002)
        discard;

    return float4(input.color.rgb, input.color.a * alpha);
}
