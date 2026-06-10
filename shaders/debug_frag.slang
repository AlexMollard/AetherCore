// Debug wireframe renderer - fragment shader
//
// Interpolated per-fragment:
//   float4 color : TEXCOORD0

struct PSInput
{
    float4 position : SV_Position;
    float4 color    : TEXCOORD0;
};

[shader("fragment")]
float4 main(PSInput input) : SV_Target0
{
    return input.color;
}
