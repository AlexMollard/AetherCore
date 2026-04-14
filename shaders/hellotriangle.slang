// Bindless texture array — set 0, binding 0.
// Texture2D supports both Load() and Sample() with a SamplerState.
[[vk::binding(0, 0)]] Texture2D g_textures[];

// Immutable linear sampler — set 0, binding 1.
// Declared as a sampler-only descriptor (not CombinedImageSampler).
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

// Per-frame camera data accessed via Buffer Device Address.
// No descriptor set required — the address is pushed in the push constant block.
// Slang supports native C-style pointer types for BDA: cast uint64_t to T*.
struct FrameConstantsData
{
    float4x4 viewProj;
    float4x4 view;
    float4x4 proj;
};

// Push constant block — matches aether::DrawPushConstants (80 bytes):
//   offset  0 : float4x4  model      (64 bytes)
//   offset 64 : uint64_t  frameAddr  ( 8 bytes)
//   offset 72 : uint       albedoSlot ( 4 bytes)
//   offset 76 : uint       _pad0      ( 4 bytes)
struct PushData
{
    float4x4 model;
    uint64_t frameAddr;
    uint     albedoSlot;
    uint     _pad0;
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
    // 0xFFFFFFFF means "no texture" — fall back to per-vertex colour.
    const uint kNoTexture = 0xFFFFFFFFu;
    float3 albedo = (pc.albedoSlot != kNoTexture)
        ? g_textures[pc.albedoSlot].Sample(g_linearSampler, input.uv).rgb
        : input.color;

    return float4(albedo, 1.0);
}
