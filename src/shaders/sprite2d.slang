#include "include/DevicePointer.slangh"
#include "include/FrameConstants.slangh"

[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

struct SpriteInstance
{
    [[vk::offset(0)]] float4x4 world;
    [[vk::offset(64)]] float4 uvRect;
    [[vk::offset(80)]] float4 color;
    [[vk::offset(96)]] float4 sizeAndPivot;
    [[vk::offset(112)]] uint64_t sortKey;
    [[vk::offset(120)]] uint textureIndex;
    [[vk::offset(124)]] uint entityId;
    [[vk::offset(128)]] uint flags;
    [[vk::offset(132)]] uint blendMode;
};

struct SpritePush
{
    DevicePtr<FrameConstantsData> frameConstants;
    DevicePtr<SpriteInstance> instances;
    float2 viewportSize;
    uint instanceOffset;
    uint padding;
};

[[vk::push_constant]] SpritePush pc;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
    nointerpolation uint textureIndex : TEXCOORD2;
    nointerpolation uint blendMode : TEXCOORD3;
    nointerpolation uint flags : TEXCOORD4;
};

[shader("vertex")]
VSOutput vertexMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    const float2 corners[6] = {
        float2(0.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0),
        float2(0.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0),
    };
    const SpriteInstance sprite = pc.instances.Ptr()[pc.instanceOffset + instanceId];
    FrameConstantsData* frame = pc.frameConstants.Ptr();
    float2 corner = corners[vertexId % 6u];
    float2 local = (corner - sprite.sizeAndPivot.zw) * sprite.sizeAndPivot.xy;
    const float4 worldPos = mul(sprite.world, float4(local, 0.0, 1.0));
    float4 clip = mul(frame->viewProj, worldPos);

    if ((sprite.flags & 4u) != 0u)
    {
        const float2 viewport = max(float2(1.0, 1.0), pc.viewportSize);
        float2 ndc = clip.xy / clip.w;
        float2 pixel = (ndc * 0.5 + 0.5) * viewport;
        ndc = (round(pixel) / viewport) * 2.0 - 1.0;
        clip.xy = ndc * clip.w;
    }

    if ((sprite.flags & 1u) != 0u) corner.x = 1.0 - corner.x;
    if ((sprite.flags & 2u) != 0u) corner.y = 1.0 - corner.y;

    VSOutput output;
    output.position = clip;
    // uvRect is in image space (origin top-left, v down); quad corners are
    // world space (+y up). Flip v so the top of the image lands on the top of
    // the quad.
    output.uv = lerp(sprite.uvRect.xy, sprite.uvRect.zw, float2(corner.x, 1.0 - corner.y));
    output.color = sprite.color;
    output.textureIndex = sprite.textureIndex;
    output.blendMode = sprite.blendMode;
    output.flags = sprite.flags;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    float2 uv = input.uv;
    if ((input.flags & 16u) != 0u)
    {
        // NearestFilter: snap to texel centres so pixel art stays crisp under
        // the shared linear sampler.
        uint width, height;
        g_textures[input.textureIndex].GetDimensions(width, height);
        const float2 texSize = float2(max(width, 1u), max(height, 1u));
        uv = (floor(uv * texSize) + 0.5) / texSize;
    }
    float4 color = g_textures[input.textureIndex].Sample(g_linearSampler, uv) * input.color;
    if (input.blendMode == 2u)
    {
        // The multiply pipeline uses DstColor/Zero. Folding alpha into the
        // source makes transparent texels neutral instead of painting black.
        color.rgb = lerp(float3(1.0, 1.0, 1.0), color.rgb, color.a);
    }
    return color;
}
