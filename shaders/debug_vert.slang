// Debug wireframe renderer - vertex shader
//
// Push constants (64 bytes at offset 0):
//   float4x4 mvp  (model-view-projection matrix)

struct DebugPushConstants
{
    float4x4 mvp;
};

[[vk::push_constant]] DebugPushConstants pc;

struct VSInput
{
    float3 position : TEXCOORD0;
};

struct VSOutput
{
    float4 position : SV_Position;
};

[shader("vertex")]
VSOutput main(VSInput input)
{
    VSOutput output;
    output.position = mul(pc.mvp, float4(input.position, 1.0));
    return output;
}