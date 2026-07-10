// Minimal project shader used to verify the editor's ShaderCompiler +
// shaders:// project overlay layer (Phase 3 of the shader asset pipeline).
// Self-contained (no includes) so it compiles independent of the engine
// shader include tree under shaders://include/.

struct VSInput
{
    float3 position : TEXCOORD0;
};

struct VSOutput
{
    float4 position : SV_Position;
};

[shader("vertex")]
VSOutput vertexMain(VSInput input)
{
    VSOutput output;
    output.position = float4(input.position, 1.0);
    return output;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    return float4(1.0, 0.0, 1.0, 1.0);
}
