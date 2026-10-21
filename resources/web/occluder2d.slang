struct MaskFrame
{
    float4 view0; float4 view1; float4 view2; float4 view3;
    float4 inverse0; float4 inverse1; float4 inverse2; float4 inverse3;
    float4 viewportCountMask; float4 ambient; float4 shadowParams; float4 shaftParams;
};
struct Occluder
{
    float4 posHalfSize;
    float4 uvRect;
    uint textureIndex; uint flags; uint pad0; uint pad1;
};
[[vk::binding(0, 0)]] ConstantBuffer<MaskFrame> frame;
[[vk::binding(1, 0)]] StructuredBuffer<Occluder> occluders;
[[vk::binding(0, 1)]] Texture2D<float4> artwork;
[[vk::binding(1, 1)]] SamplerState linearSampler;
struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint flags : TEXCOORD1;
    nointerpolation float4 capsule : TEXCOORD2;
    nointerpolation float radius : TEXCOORD3;
};
[shader("vertex")]
VertexOutput vertexMain(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    const float2 corners[6] = {float2(0,0),float2(1,0),float2(1,1),float2(0,0),float2(1,1),float2(0,1)};
    float2 corner = corners[vertex];
    const Occluder occluder = occluders[instance];
    VertexOutput output;
    output.flags = occluder.flags;
    output.capsule = float4(occluder.posHalfSize.xy, occluder.uvRect.xy);
    output.radius = occluder.posHalfSize.z;
    float2 world;
    if ((occluder.flags & 4u) != 0u)
    {
        world = lerp(min(output.capsule.xy, output.capsule.zw) - output.radius,
            max(output.capsule.xy, output.capsule.zw) + output.radius, corner);
        output.uv = world;
    }
    else
    {
        world = occluder.posHalfSize.xy + (corner * 2.0 - 1.0) * output.radius;
        if ((output.flags & 1u) != 0u) { corner.x = 1.0 - corner.x; }
        if ((output.flags & 2u) != 0u) { corner.y = 1.0 - corner.y; }
        output.uv = lerp(occluder.uvRect.xy, occluder.uvRect.zw, float2(corner.x, 1.0 - corner.y));
    }
    const float4x4 viewProj = float4x4(frame.view0, frame.view1, frame.view2, frame.view3);
    output.position = mul(viewProj, float4(world, 0.0, 1.0));
    output.position.y = -output.position.y;
    return output;
}
[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target0
{
    if ((input.flags & 4u) != 0u)
    {
        const float2 pa = input.uv - input.capsule.xy;
        const float2 ba = input.capsule.zw - input.capsule.xy;
        const float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-6), 0.0, 1.0);
        if (length(pa - ba * h) > input.radius) { discard; }
        return float4(1.0, 0.0, 0.0, 0.0);
    }
    const float alpha = artwork.SampleLevel(linearSampler, input.uv, 0.0).a;
    if (alpha < 0.35) { discard; }
    return float4(alpha, 0.0, 0.0, 0.0);
}
