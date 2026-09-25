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
// Layout, target 0, RGBA8:
//   RG = world normal, octahedral-encoded
//   B  = roughness
//   A  = metallic
//
// Target 1, RGBA8 sRGB: base colour.
//
// That second target exists for one reason. A metal's Fresnel reflectance is its
// base colour - gold reflects gold - so the forward pass weights its sky specular
// by lerp(0.04, baseColour, metallic), a COLOUR. The reflection composite has to
// subtract exactly what the forward pass added, and without base colour it could
// only guess with a scalar, over-subtracting every channel where the metal is dark.
// On copper that removes about half the blue the sky put there.
//
// Base colour rather than F0 itself, deliberately: storing F0 would quantise a
// dielectric's 0.04 to 10/255, and the subtraction would stop cancelling on the
// floors and walls that make up most of the screen. Reconstructing
// lerp(0.04, base, metallic) in the composite keeps metallic == 0 exact no matter
// what the texel holds.
//
// sRGB rather than Unorm, because base colour is perceptual data: the hardware
// encode spends its 8 bits where the eye needs them and hands back linear on read.
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
#include "include/AlphaMaskTest.slangh"

struct PrepassVSOutput
{
	float4 svPosition    : SV_Position;
	float3 worldNormal   : TEXCOORD0;
	float2 uv            : TEXCOORD1;
	nointerpolation uint materialIndex : TEXCOORD2;
	float3 vertexColor   : TEXCOORD3;
};

struct PrepassOutput
{
	float4 surface   : SV_Target0;
	float4 baseColor : SV_Target1;
};

[shader("vertex")]
PrepassVSOutput vertexMain(uint vertexId : SV_VertexID, uint startInstance : SV_StartInstanceLocation)
{
	float3 worldPos;
	float2 depthUv;
	uint depthMaterialIndex;
	const float4 clipPos = DepthVertexTransform(vertexId, startInstance, worldPos, depthUv, depthMaterialIndex);

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
	o.vertexColor = v.color;
	return o;
}

[shader("fragment")]
PrepassOutput fragmentMain(PrepassVSOutput input, bool isFrontFace : SV_IsFrontFace)
{
	AlphaMaskDiscard(input.materialIndex, input.uv);

	FrameConstantsData* fc = pc.frame.Ptr();
	GpuMaterial* materials = (GpuMaterial*)(fc->materialBufferAddr);

	float roughness = 1.0f;
	float metallic = 0.0f;
	float3 baseColor = float3(1.0f, 1.0f, 1.0f);
	const bool hasMaterial = input.materialIndex != kNoTexture;
	if (hasMaterial)
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

		baseColor = mat.baseColorFactor.rgb;
		if (mat.albedoSlot != kNoTexture)
		{
			baseColor *= g_textures[mat.albedoSlot].Sample(g_linearSampler, input.uv).rgb;
		}
	}

	// Vertex colour, gated and sanitised exactly as the forward shader gates it. The
	// two have to agree term for term or the composite subtracts something the
	// forward pass never added.
	float3 vertexColor = float3(1.0f, 1.0f, 1.0f);
	if (!hasMaterial || (materials[input.materialIndex].flags & kFlagModulateVertexColor) != 0u)
	{
		vertexColor = input.vertexColor;
	}
	if (any(vertexColor < 0.0f) || any(vertexColor > 1.0f) || dot(vertexColor, vertexColor) < 0.0025f)
	{
		vertexColor = float3(1.0f, 1.0f, 1.0f);
	}
	baseColor *= vertexColor;

	// Flipped for a back face, exactly as the forward shader does. This buffer is what GTAO
	// reads, and a normal pointing away from the camera makes the occlusion integral decide
	// the surface is facing into the geometry behind it - so a two-sided surface seen from
	// behind came out almost fully occluded and lost its ambient entirely. The two have to
	// agree: shading one normal while occluding another is not a subtle disagreement.
	const float3 N = normalize(input.worldNormal) * (isFrontFace ? 1.0f : -1.0f);

	// The forward shader does not shade with the roughness the material declares - it
	// floors it, then widens it by however much the normal varies across the pixel
	// (geometric specular antialiasing). Everything reading this buffer has to see the
	// SAME number, or it describes a surface the renderer never shaded.
	//
	// The reflection composite is the sharp end of that: it subtracts the sky the forward
	// pass added, and if the two disagree about roughness the subtraction leaves a
	// residue.
	//
	// How much it can diverge is bounded by the widening cap of 0.18, which would take a
	// roughness of 0.05 to 0.43 - but that is the ceiling, not a measurement. On the test
	// scene the correction moves nothing above the noise floor, because smooth spheres at
	// a few metres barely vary their normal across a pixel. It is worth doing anyway: the
	// cost is nothing, and the variance climbs with distance and with geometric detail,
	// which is exactly where a mirror-sharp reflection on a surface being shaded as matte
	// would be most obvious.
	//
	// Reproduced term for term from gltf_mesh.slang. The one difference left is that this
	// pass has no normal map, so the variance it measures is the geometry's alone; a
	// normal-mapped surface still widens slightly more in the forward pass than here.
	float shadedRoughness = clamp(roughness, 0.04f, 1.0f);
	{
		const float3 dNdx = ddx(N);
		const float3 dNdy = ddy(N);
		const float kSigma2 = 0.15915494f;
		const float kKappa = 0.18f;
		const float variance = kSigma2 * (dot(dNdx, dNdx) + dot(dNdy, dNdy));
		const float kernelRoughness = min(2.0f * variance, kKappa);
		shadedRoughness = sqrt(saturate(shadedRoughness * shadedRoughness + kernelRoughness));
	}

	PrepassOutput o;
	o.surface = float4(EncodeOctNormal(N), saturate(shadedRoughness), saturate(metallic));
	o.baseColor = float4(saturate(baseColor), 1.0f);
	return o;
}
