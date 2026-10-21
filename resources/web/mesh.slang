// Unlit material surface adapter. Lighting/PBR/effect parity is a later pass.
struct FrameData
{
	float4 clip0, clip1, clip2, clip3;
	float4 time;
};
struct MeshData
{
	float4 world0, world1, world2, world3;
	float4 normal0, normal1, normal2;
	float4 color;
	float4 emissiveRoughness;
	float4 factors;
	float4 uvScroll;
	uint4 metadata;
};
struct SkinData { float4 c0, c1, c2, c3; };
[[vk::binding(0, 0)]] ConstantBuffer<FrameData> frame;
[[vk::binding(1, 0)]] StructuredBuffer<MeshData> meshes;
[[vk::binding(2, 0)]] StructuredBuffer<SkinData> skin;
[[vk::binding(0, 1)]] Texture2D<float4> albedo;
[[vk::binding(1, 1)]] Texture2D<float4> emissive;
[[vk::binding(2, 1)]] SamplerState materialSampler;
struct VertexOutput
{
	float4 position : SV_Position;
	float4 color : COLOR0;
	float2 uv : TEXCOORD0;
	nointerpolation uint instance : TEXCOORD1;
};
[shader("vertex")]
VertexOutput vertexMain(float3 position : POSITION, float3 color : COLOR0, float2 uv : TEXCOORD0, uint4 joints : BLENDINDICES, float4 weights : BLENDWEIGHT, uint instance : SV_InstanceID)
{
	MeshData mesh = meshes[instance];
	float4 local = float4(position, 1);
	if (mesh.metadata.z != 0)
	{
		local = float4(0);
		for (uint i = 0; i < 4; ++i)
		{
			if (weights[i] > 0 && joints[i] < mesh.metadata.z)
			{
				SkinData bone = skin[mesh.metadata.y + joints[i]];
				local += (bone.c0 * position.x + bone.c1 * position.y + bone.c2 * position.z + bone.c3) * weights[i];
			}
		}
	}
	float4 world = mesh.world0 * local.x + mesh.world1 * local.y + mesh.world2 * local.z + mesh.world3 * local.w;
	VertexOutput output;
	output.position = float4(dot(frame.clip0, world), dot(frame.clip1, world), dot(frame.clip2, world), dot(frame.clip3, world));
	output.position.y = -output.position.y;
	output.color = mesh.color * ((mesh.metadata.w & 8) != 0 ? float4(color, 1) : float4(1));
	output.uv = uv + mesh.uvScroll.xy * frame.time.x;
	output.instance = instance;
	return output;
}
float3 EncodeSrgb(float3 value)
{
	value = max(value, float3(0));
	return select(value <= float3(0.0031308), value * 12.92, 1.055 * pow(value, float3(1.0 / 2.4)) - 0.055);
}
[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
	MeshData mesh = meshes[input.instance];
	float4 surface = input.color * albedo.Sample(materialSampler, input.uv);
	float3 emission = mesh.emissiveRoughness.xyz * emissive.Sample(materialSampler, input.uv).xyz;
	if ((mesh.metadata.w & 4) != 0 && surface.a < mesh.factors.z) { discard; }
	return float4(EncodeSrgb(surface.rgb + emission), surface.a);
}
