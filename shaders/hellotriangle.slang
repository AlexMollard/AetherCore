// Per-frame camera data accessed via Buffer Device Address.
// No descriptor set required — the address is pushed in the push constant block.
// Slang supports native C-style pointer types for BDA: cast uint64_t to T*.
struct FrameConstantsData
{
    float4x4 viewProj;
    float4x4 view;
    float4x4 proj;
};

// Push constant block — matches meow::DrawPushConstants (72 bytes):
//   offset  0 : float4x4  model      (64 bytes)
//   offset 64 : uint64_t  frameAddr  ( 8 bytes)
struct PushData
{
    float4x4 model;
    uint64_t frameAddr;
};

[[vk::push_constant]] PushData pc;

struct VSOutput
{
    float4 position  : SV_Position;
    float3 normal    : NORMAL;
    float4 tangent   : TANGENT;
    float2 uv        : TEXCOORD0;
    float3 color     : COLOR0;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 tangent  : TANGENT;
    float2 uv       : TEXCOORD0;
    float3 color    : COLOR0;
};

[shader("vertex")]
VSOutput vertexMain(VSInput input)
{
    // Dereference the BDA to read per-frame camera data.
    FrameConstantsData* fc = (FrameConstantsData*)(pc.frameAddr);

    VSOutput output;
    float4 worldPos     = mul(pc.model, float4(input.position, 1.0));
    output.position     = mul(fc->viewProj, worldPos);
    output.normal       = input.normal;
    output.tangent      = input.tangent;
    output.uv           = input.uv;
    output.color        = input.color;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    return float4(input.color, 1.0);
}
