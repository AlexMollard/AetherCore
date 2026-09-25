#include "include/DevicePointer.slangh"
#include "include/FrameConstants.slangh"
#include "include/ParticleBillboard.slangh"

[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

struct BillboardPush
{
    DevicePtr<FrameConstantsData> frameConstants;
    DevicePtr<GpuBillboardParticle> instances;
    uint instanceOffset;
    uint padding;
};

[[vk::push_constant]] BillboardPush pc;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
    nointerpolation uint textureIndex : TEXCOORD2;
};

[shader("vertex")]
VSOutput vertexMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    const float2 corners[6] = {
        float2(0.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0),
        float2(0.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0),
    };
    const GpuBillboardParticle p = pc.instances.Ptr()[pc.instanceOffset + instanceId];
    FrameConstantsData* frame = pc.frameConstants.Ptr();
    const float2 corner = corners[vertexId % 6u];

    // Centre in view space, expand on the view plane there, then project. Working in view
    // space makes the billboarding exact without depending on the view matrix's storage
    // layout: +x is screen right and +y is screen up by definition.
    const float3 centreView = mul(frame->view, float4(p.positionSize.xyz, 1.0)).xyz;
    float2 local = (corner - 0.5) * p.positionSize.w;
    const float radians = p.rotationDegrees * 3.14159265 / 180.0;
    const float s = sin(radians);
    const float c = cos(radians);
    local = float2(local.x * c - local.y * s, local.x * s + local.y * c);
    const float4 clip = mul(frame->proj, float4(centreView + float3(local, 0.0), 1.0));

    VSOutput output;
    output.position = clip;
    // uvRect is in image space (origin top-left, v down); quad corners are view space
    // (+y up). Flip v so the top of the image lands on the top of the quad.
    output.uv = lerp(p.uvRect.xy, p.uvRect.zw, float2(corner.x, 1.0 - corner.y));
    output.color = p.color;
    output.textureIndex = p.textureIndex;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    // Particle pages are chunky pixel art drawn point-sampled on the PS2: snap to texel
    // centres so the shared linear sampler cannot blur them.
    uint width, height;
    g_textures[input.textureIndex].GetDimensions(width, height);
    const float2 texSize = float2(max(width, 1u), max(height, 1u));
    const float2 uv = (floor(input.uv * texSize) + 0.5) / texSize;
    return g_textures[input.textureIndex].Sample(g_linearSampler, uv) * input.color;
}
