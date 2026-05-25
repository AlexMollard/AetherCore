// local_shadow_depth.slang - VSM shadow caster for local lights.
// Outputs float2(depth, depth*depth) to color attachment.
// Vertex data fetched via BDA (DrawInstanceData.vertexBufferAddr).
// Light VP matrix comes from per-light FrameConstants via BDA in push constants

#include "include/FrameConstants.slangh"
#include "include/RenderContracts.slangh"

[[vk::push_constant]] DrawPushConstants pc;

struct Vertex
{
    [[vk::offset(0)]]  float3 position;
    [[vk::offset(12)]] float3 normal;
    [[vk::offset(24)]] float4 tangent;
    [[vk::offset(40)]] float2 uv;
    [[vk::offset(48)]] float3 color;
    [[vk::offset(60)]] uint4  jointIndices;
    [[vk::offset(76)]] float4 jointWeights;
};

struct VSOutput
{
    float4 svPosition : SV_Position;
    float3 worldPos   : TEXCOORD0;
};

[shader("vertex")]
VSOutput vertexMain(uint vertexId : SV_VertexID, uint startInstance : SV_StartInstanceLocation)
{
    FrameConstantsData* fc = (FrameConstantsData*)(pc.frameAddr);
    DrawInstanceData* instances = (DrawInstanceData*)(pc.instanceDataAddr);
    const uint instanceIndex = startInstance;
    DrawInstanceData instance = instances[instanceIndex];

    Vertex* verts = (Vertex*)(instance.vertexBufferAddr);
    Vertex input = verts[vertexId];

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
    o.worldPos = worldPos.xyz;
    o.svPosition = mul(fc->viewProj, worldPos);
    return o;
}

[shader("fragment")]
float2 fragmentMain(VSOutput input) : SV_Target
{
    float depth = input.svPosition.z;
    return float2(depth, depth * depth);
}
