// Debug wireframe renderer - fragment shader
//
// Push constants (at offset 64):
//   float4 color  (RGBA)

struct DebugPushConstants
{
    float4x4 mvp;
    float4 color;
};

[[vk::push_constant]] DebugPushConstants pc;

struct PSInput
{
    float4 position : SV_Position;
};

[shader("fragment")]
float4 main(PSInput input) : SV_Target0
{
    return pc.color;
}