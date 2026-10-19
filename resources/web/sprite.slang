struct FrameData
{
    float4 clip0;
    float4 clip1;
    float4 clip2;
    float4 clip3;
    float4 extent;
};
struct SpriteData
{
    float4 world0;
    float4 world1;
    float4 world2;
    float4 world3;
    float4 uvRect;
    float4 color;
    float4 sizeAndPivot;
    uint4 metadata;
};
[[vk::binding(0, 0)]] ConstantBuffer<FrameData> frame;
[[vk::binding(1, 0)]] StructuredBuffer<SpriteData> sprites;
[[vk::binding(2, 0)]] Texture2D<float4> spriteTexture;
[[vk::binding(3, 0)]] SamplerState spriteSampler;
struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    nointerpolation uint flags : TEXCOORD1;
};
[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    const float2 corners[6] = {float2(0,0),float2(1,0),float2(0,1),float2(0,1),float2(1,0),float2(1,1)};
    SpriteData sprite = sprites[instanceId];
    float2 uv = corners[vertexId];
    float2 local = (uv - sprite.sizeAndPivot.zw) * sprite.sizeAndPivot.xy;
    float4 world = sprite.world0 * local.x + sprite.world1 * local.y + sprite.world3;
    VertexOutput output;
    output.position = float4(dot(frame.clip0,world),dot(frame.clip1,world),dot(frame.clip2,world),dot(frame.clip3,world));
    // Render packets use Vulkan clip Y; WebGPU NDC has +Y up.
    output.position.y = -output.position.y;
    if ((sprite.metadata.x & 4) != 0)
    {
        float2 pixel = (output.position.xy / output.position.w * 0.5 + 0.5) * frame.extent.xy;
        output.position.xy = (round(pixel) / frame.extent.xy * 2 - 1) * output.position.w;
    }
    if ((sprite.metadata.x & 1) != 0) { uv.x = 1 - uv.x; }
    if ((sprite.metadata.x & 2) != 0) { uv.y = 1 - uv.y; }
    output.uv = lerp(sprite.uvRect.xy,sprite.uvRect.zw,float2(uv.x,1-uv.y));
    output.color = sprite.color;
    output.flags = sprite.metadata.x;
    return output;
}
[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    float2 uv = input.uv;
    if ((input.flags & 16) != 0)
    {
        uint width, height;
        spriteTexture.GetDimensions(width,height);
        float2 size = float2(width,height);
        uv = (floor(uv * size) + 0.5) / size;
    }
    float4 color = spriteTexture.Sample(spriteSampler,uv) * input.color;
    if ((input.flags & 8) != 0 && color.a < 0.5) { discard; }
    return color;
}
