// SDF text rendering shader.
//
// Vertices are generated procedurally from SV_VertexID (6 per glyph quad).
// The host supplies one TextGlyphPush per vkCmdDraw(6) call.
//
// Atlas format: R8_UNORM single-channel SDF where:
//   0.0 = far outside glyph, 0.5 = glyph edge, 1.0 = far inside glyph.
//
// Anti-aliasing: screen-space derivative (fwidth) of the SDF value is used
// to set a per-pixel smooth transition width so edges stay crisp at any size.

// Bindless texture array — set 0, binding 0.
[[vk::binding(0, 0)]] Texture2D g_textures[];

// Immutable linear sampler — set 0, binding 1.
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

// Per-glyph push constants (72 bytes):
//   offset  0 : float4  screenSize  (16) — xy = viewport pixels, zw unused
//   offset 16 : float4  glyphRect   (16) — x, y, w, h  in screen pixels (top-left origin)
//   offset 32 : float4  uvRect      (16) — u0, v0, u1, v1 in [0, 1] atlas space
//   offset 48 : float4  color       (16) — RGBA tint (linear)
//   offset 64 : uint    atlasSlot   ( 4) — bindless sampled-image index
//   offset 68 : uint    _pad0       ( 4) — explicit padding
// Total: 72 bytes.
struct TextGlyphPush
{
    float4 screenSize;  // .xy = viewport dimensions, .zw unused
    float4 glyphRect;
    float4 uvRect;
    float4 color;
    uint   atlasSlot;
    uint   _pad0;
};

[[vk::push_constant]] TextGlyphPush pc;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

[shader("vertex")]
VSOutput vertexMain(uint vertexId : SV_VertexID)
{
    // Glyph quad corners in screen pixels, top-left origin.
    // Two CCW triangles: (0,1,2) and (0,2,3).
    //   3 --- 2
    //   |   / |
    //   |  /  |
    //   | /   |
    //   0 --- 1
    const float x0 = pc.glyphRect.x;
    const float y0 = pc.glyphRect.y;
    const float x1 = pc.glyphRect.x + pc.glyphRect.z;
    const float y1 = pc.glyphRect.y + pc.glyphRect.w;

    const float2 positions[6] = {
        float2(x0, y1),   // 0 bottom-left
        float2(x1, y1),   // 1 bottom-right
        float2(x1, y0),   // 2 top-right
        float2(x0, y1),   // 3 bottom-left  (second triangle)
        float2(x1, y0),   // 4 top-right
        float2(x0, y0),   // 5 top-left
    };

    const float u0 = pc.uvRect.x;
    const float v0 = pc.uvRect.y;
    const float u1 = pc.uvRect.z;
    const float v1 = pc.uvRect.w;

    const float2 uvs[6] = {
        float2(u0, v1),
        float2(u1, v1),
        float2(u1, v0),
        float2(u0, v1),
        float2(u1, v0),
        float2(u0, v0),
    };

    const float2 sp = positions[vertexId];

    // Convert screen-pixel coords to Vulkan NDC: x in [-1,+1], y in [-1,+1]
    // with (0,0) at top-left in pixel space and (-1,-1) at top-left in NDC.
    const float ndcX = (sp.x / pc.screenSize.x) * 2.0f - 1.0f;
    const float ndcY = (sp.y / pc.screenSize.y) * 2.0f - 1.0f;

    VSOutput output;
    output.position = float4(ndcX, ndcY, 0.0, 1.0);
    output.uv       = uvs[vertexId];
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

    return float4(pc.color.rgb, pc.color.a * alpha);
}
