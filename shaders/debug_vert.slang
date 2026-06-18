// Debug wireframe renderer - vertex shader
//
// Push constants (88 bytes at offset 0):
//   offset  0 : DevicePtr<FrameConstantsData> frame  (viewProj lives here)
//   offset  8 : float4    tintColor   multiplies into per-vertex color
//   offset 24 : float4x4  model       per-draw world transform
//
// Per-vertex input:
//   float3 position : TEXCOORD0
//   float4 color    : TEXCOORD1
//
// Diagnostic: tintColor.w == 2.0 -> bypass viewProj, treat input.position as NDC.

#include "include/DevicePointer.slangh"
#include "include/FrameConstants.slangh"

struct DebugPushConstants
{
    DevicePtr<FrameConstantsData> frame;
    float4   tintColor;
    float4x4 model;
};

[[vk::push_constant]] DebugPushConstants pc;

struct VSInput
{
    float3 position : TEXCOORD0;
    float4 color    : TEXCOORD1;
};

struct VSOutput
{
    float4 position : SV_Position;
    float4 color    : TEXCOORD0;
};

[shader("vertex")]
VSOutput main(VSInput input)
{
    VSOutput output;
    if (pc.tintColor.w >= 1.5)
    {
        output.position = float4(input.position, 1.0);
        output.color = float4(input.color.rgb * pc.tintColor.rgb, 1.0);
    }
    else
    {
        FrameConstantsData* fc = pc.frame.Ptr();
        output.position = mul(fc->viewProj, mul(pc.model, float4(input.position, 1.0)));
        output.color = float4(input.color.rgb * pc.tintColor.rgb, input.color.a);
    }
    return output;
}
