// Solid authored effect used by the UI parity reference scene.
struct EffectPush { float4 screenSize; float4 rect; float4 params; float4 color0; float4 color1; };
[[vk::push_constant]] EffectPush pc;
struct VSOutput { float4 position : SV_Position; float2 uv : TEXCOORD0; };
[shader("vertex")]
VSOutput vertexMain(uint vertexId : SV_VertexID)
{
    const float2 vertices[6] = { float2(0,1), float2(1,1), float2(1,0), float2(0,1), float2(1,0), float2(0,0) };
    const float2 uv = vertices[vertexId % 6u];
    const float2 p = pc.rect.xy + uv * pc.rect.zw;
    VSOutput output;
    output.position = float4(p.x / pc.screenSize.x * 2.0 - 1.0, p.y / pc.screenSize.y * 2.0 - 1.0, 0.0, 1.0);
    output.uv = uv;
    return output;
}
[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0 { return pc.color0; }
