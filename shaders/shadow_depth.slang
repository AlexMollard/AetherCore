// shadow_depth.slang - depth-only shadow caster for directional light.

#include "include/FrameConstants.slangh"
#include "include/RenderContracts.slangh"

[[vk::push_constant]] DrawPushConstants pc;

struct VSInput
{
    [[vk::location(0)]] float3 position     : POSITION;
    [[vk::location(1)]] float3 normal       : NORMAL;
    [[vk::location(2)]] float4 tangent      : TANGENT;
    [[vk::location(3)]] float2 uv           : TEXCOORD0;
    [[vk::location(4)]] float3 color        : COLOR0;
    [[vk::location(5)]] uint4  jointIndices : BLENDINDICES0;
    [[vk::location(6)]] float4 jointWeights : BLENDWEIGHT0;
};

struct VSOutput
{
    float4 svPosition : SV_Position;
};

[shader("vertex")]
VSOutput vertexMain(VSInput input, uint instanceId : SV_InstanceID, uint startInstance : SV_StartInstanceLocation)
{
    FrameConstantsData* fc = (FrameConstantsData*)(pc.frameAddr);
    DrawInstanceData* instances = (DrawInstanceData*)(pc.instanceDataAddr);
    const uint instanceIndex = startInstance + instanceId;
    DrawInstanceData instance = instances[instanceIndex];

    float4 worldPos;
    if (pc.skinPaletteAddr != 0uL && instance.skinJointCount > 0u)
    {
        float4x4* joints = (float4x4*)(pc.skinPaletteAddr);
        const uint base = instance.skinPaletteOffset;
        float4x4 skinMatrix =
            input.jointWeights.x * joints[base + input.jointIndices.x] +
            input.jointWeights.y * joints[base + input.jointIndices.y] +
            input.jointWeights.z * joints[base + input.jointIndices.z] +
            input.jointWeights.w * joints[base + input.jointIndices.w];

        worldPos = mul(instance.model, mul(skinMatrix, float4(input.position, 1.0)));
    }
    else
    {
        worldPos = mul(instance.model, float4(input.position, 1.0));
    }

    VSOutput o;
    o.svPosition = mul(fc->viewProj, worldPos);
    return o;
}

[shader("fragment")]
void fragmentMain()
{
}
