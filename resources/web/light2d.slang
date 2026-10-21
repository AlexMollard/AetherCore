struct Light2DFrame
{
    float4 view0; float4 view1; float4 view2; float4 view3;
    float4 inverse0; float4 inverse1; float4 inverse2; float4 inverse3;
    float4 viewportCountMask;
    float4 ambient;
    float4 shadowParams;
    float4 shaftParams;
};
struct GpuLight2D
{
    float4 posRadiusKind;
    float4 colorIntensity;
    float4 spotDirCone;
    float4 flags;
};
[[vk::binding(0, 0)]] ConstantBuffer<Light2DFrame> frame;
[[vk::binding(1, 0)]] StructuredBuffer<GpuLight2D> lights;
[[vk::binding(2, 0)]] Texture2D<float> mask;
[[vk::binding(3, 0)]] SamplerState linearSampler;
float SampleOccluder(float2 uv) { return mask.SampleLevel(linearSampler, uv, 0.0); }
GpuLight2D LoadLight2D(uint index) { return lights[index]; }
#include "../../src/shaders/include/Light2DMath.slangh"
struct VertexOutput { float4 position : SV_Position; };
[shader("vertex")]
VertexOutput vertexMain(uint index : SV_VertexID)
{
    const float2 corners[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    VertexOutput output;
    output.position = float4(corners[index], 0.0, 1.0);
    return output;
}
[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target0
{
    const float4x4 viewProj = float4x4(frame.view0, frame.view1, frame.view2, frame.view3);
    const float4x4 inverse = float4x4(frame.inverse0, frame.inverse1, frame.inverse2, frame.inverse3);
    // The packet projection uses native Y-down clip space; browser rasterization uses Y-up.
    const float2 uv = input.position.xy / frame.viewportCountMask.xy;
    return EvaluateLight2D(uv, viewProj, inverse, 0.0, uint(frame.viewportCountMask.z), frame.ambient,
        frame.shadowParams, frame.shaftParams, frame.viewportCountMask.w > 0.5);
}
