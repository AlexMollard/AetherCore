// imgui.slang - Dear ImGui custom Vulkan backend shader.
// Vertex data is pulled via BDA (Buffer Device Address) to stay consistent
// with the rest of the engine's bindless/BDA rendering approach.
// Font and user textures are sampled through the global bindless set (set 0).

[vk::binding(0, 0)] Texture2D<float4> g_textures[];
[vk::binding(1, 0)] SamplerState       g_linearSampler;

// Must match ImDrawVert layout exactly (20 bytes, no padding).
struct ImDrawVert
{
    float2 pos; // offset  0
    float2 uv;  // offset  8
    uint   col; // offset 16  - packed R8G8B8A8 (R in LSB)
};

struct PushConstants
{
    float2   scale;       // orthographic scale  (2/displayW, 2/displayH)
    float2   translate;   // orthographic offset (-1, -1)
    uint64_t vtxAddr;     // BDA of the vertex buffer for this frame slot
    uint     textureSlot; // bindless slot for the current draw command's texture
    uint     _pad;
};

[[vk::push_constant]]
PushConstants g_push;

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

[shader("vertex")]
VSOut vertexMain(uint vid : SV_VertexID)
{
    ImDrawVert* buf = (ImDrawVert*)g_push.vtxAddr;
    ImDrawVert  v   = buf[vid];

    // Unpack ImGui's R8G8B8A8 packed colour (R in the lowest byte).
    float4 col;
    col.r = float((v.col      ) & 0xFFu) / 255.0;
    col.g = float((v.col >>  8) & 0xFFu) / 255.0;
    col.b = float((v.col >> 16) & 0xFFu) / 255.0;
    col.a = float((v.col >> 24) & 0xFFu) / 255.0;

    VSOut o;
    o.position = float4(v.pos * g_push.scale + g_push.translate, 0.0, 1.0);
    o.uv       = v.uv;
    o.color    = col;
    return o;
}

[shader("fragment")]
float4 fragmentMain(VSOut i) : SV_Target
{
    return g_textures[g_push.textureSlot].Sample(g_linearSampler, i.uv) * i.color;
}
