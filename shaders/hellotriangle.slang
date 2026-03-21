struct VSOutput
{
    float4 position : SV_Position;
    float3 color : COLOR0;
};

struct VSInput
{
    float3 position : POSITION;
    float3 color : COLOR0;
};

[shader("vertex")]
VSOutput vertexMain(VSInput input)
{
    VSOutput output;
    output.position = float4(input.position, 1.0);
    output.color = input.color;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    return float4(input.color, 1.0);
}
