// Depth prepass that also lays down a thin G-buffer.
//
// Screen-space effects need to know, for each pixel, which way the surface faces
// and how rough it is. Ambient occlusion currently reconstructs the normal from
// neighbouring depth samples, which is only ever an estimate and goes wrong on
// any silhouette; screen-space reflections cannot work at all without roughness,
// because a mirror and a matte surface reflect completely differently.
//
// The obvious way to produce that is to attach a second render target to the
// forward pass. It is also the wrong way here: every material pipeline and every
// project shader drawing into that pass would have to declare and write the
// extra attachment, or silently fill it with garbage. That is an engine-wide
// contract change to gain one buffer.
//
// The prepass already submits exactly the same geometry, and it runs one
// pipeline for every draw rather than one per material. Writing the G-buffer
// here costs no extra geometry submission and changes nothing any other shader
// has to know about.
//
// Layout, RGBA8:
//   RG = world normal, octahedral-encoded
//   B  = roughness
//   A  = metallic
//
// Octahedral rather than storing xy and rebuilding z: the sign of z is lost that
// way, so back-facing normals come back wrong. Octahedral mapping spends both
// channels on a bijective projection of the whole sphere, which at 8 bits per
// channel is accurate to well under a degree - far finer than the reflection
// directions built from it need.

[[vk::binding(0, 0)]] Texture2D    g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

#include "include/FrameConstants.slangh"
#include "include/RenderContracts.slangh"
#include "include/GpuMaterial.slangh"
#include "include/MeshVertex.slangh"

[[vk::push_constant]] DrawPushConstants pc;

#include "include/DepthVertex.slangh"
#include "include/OctNormal.slangh"

struct PrepassVSOutput
{
	float4 svPosition    : SV_Position;
	float3 worldNormal   : TEXCOORD0;
	float2 uv            : TEXCOORD1;
	nointerpolation uint materialIndex : TEXCOORD2;
};

[shader("vertex")]
PrepassVSOutput vertexMain(uint vertexId : SV_VertexID, uint startInstance : SV_StartInstanceLocation)
{
	float3 worldPos;
	const float4 clipPos = DepthVertexTransform(vertexId, startInstance, worldPos);

	FrameConstantsData* fc = pc.frame.Ptr();
	const DrawInstanceData instance = pc.instanceData.Load(startInstance);
	Vertex* verts = (Vertex*)(instance.vertexBufferAddr);
	const Vertex v = verts[vertexId];

	// Skinned meshes deform their normals too. Reusing the joint weights the
	// position went through keeps the G-buffer consistent with what the forward
	// pass will shade, rather than laying down the bind-pose normal.
	float3 nWorld;
	if (pc.skinPalette.addr != 0uL && instance.skinJointCount > 0u)
	{
		float4x4* joints = pc.skinPalette.Ptr();
		const uint base = instance.skinPaletteOffset;
		const uint jc = instance.skinJointCount;

		const uint  j0 = (v.jointIndices.x < jc) ? v.jointIndices.x : 0u;
		const uint  j1 = (v.jointIndices.y < jc) ? v.jointIndices.y : 0u;
		const uint  j2 = (v.jointIndices.z < jc) ? v.jointIndices.z : 0u;
		const uint  j3 = (v.jointIndices.w < jc) ? v.jointIndices.w : 0u;
		float w0 = (v.jointIndices.x < jc) ? v.jointWeights.x : 0.0f;
		float w1 = (v.jointIndices.y < jc) ? v.jointWeights.y : 0.0f;
		float w2 = (v.jointIndices.z < jc) ? v.jointWeights.z : 0.0f;
		float w3 = (v.jointIndices.w < jc) ? v.jointWeights.w : 0.0f;

		const float wsum = w0 + w1 + w2 + w3;
		if (wsum > 1e-6f)
		{
			const float inv = 1.0f / wsum;
			w0 *= inv; w1 *= inv; w2 *= inv; w3 *= inv;
		}
		else
		{
			w0 = 1.0f; w1 = 0.0f; w2 = 0.0f; w3 = 0.0f;
		}

		const float4x4 skin =
		    w0 * joints[base + j0] +
		    w1 * joints[base + j1] +
		    w2 * joints[base + j2] +
		    w3 * joints[base + j3];

		nWorld = mul((float3x3)instance.model, mul((float3x3)skin, v.normal));
	}
	else
	{
		nWorld = mul((float3x3)instance.model, v.normal);
	}

	PrepassVSOutput o;
	o.svPosition = clipPos;
	o.worldNormal = nWorld;
	o.uv = v.uv;
	o.materialIndex = instance.materialIndex;
	return o;
}

[shader("fragment")]
float4 fragmentMain(PrepassVSOutput input) : SV_Target0
{
	FrameConstantsData* fc = pc.frame.Ptr();
	GpuMaterial* materials = (GpuMaterial*)(fc->materialBufferAddr);

	float roughness = 1.0f;
	float metallic = 0.0f;
	if (input.materialIndex != kNoTexture)
	{
		const GpuMaterial mat = materials[input.materialIndex];
		roughness = mat.roughnessFactor;
		metallic = mat.metallicFactor;
		if (mat.metallicRoughnessSlot != kNoTexture)
		{
			// Same glTF ORM convention the forward shader uses. Sampling it here
			// rather than taking the factor alone matters: a surface whose map
			// says 0.2 where the factor says 1.0 reflects like a mirror, and a
			// reflection built from the factor would be uniformly wrong.
			const float3 orm = g_textures[mat.metallicRoughnessSlot].Sample(g_linearSampler, input.uv).rgb;
			roughness *= orm.g;
			metallic *= orm.b;
		}
	}

	const float3 N = normalize(input.worldNormal);
	return float4(EncodeOctNormal(N), saturate(roughness), saturate(metallic));
}
