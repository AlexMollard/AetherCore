struct FrameData
{
	float4 clipScale;
};

struct SpriteData
{
	float4 rect;
	float4 tint;
};

[[vk::binding(0, 0)]] ConstantBuffer<FrameData> frame;
[[vk::binding(1, 0)]] StructuredBuffer<SpriteData> sprites;
[[vk::binding(2, 0)]] Texture2D<float4> spriteTexture;
[[vk::binding(3, 0)]] SamplerState spriteSampler;

struct VertexOutput
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float4 tint : COLOR0;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID)
{
	const float2 corners[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1) };
	float2 uv = corners[vertexId];
	SpriteData sprite = sprites[0];
	VertexOutput result;
	result.position = float4((sprite.rect.xy + uv * sprite.rect.zw) * frame.clipScale.xy + frame.clipScale.zw, 0, 1);
	result.uv = uv;
	result.tint = sprite.tint;
	return result;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
	return spriteTexture.Sample(spriteSampler, input.uv) * input.tint;
}
