// Applies the half-res volumetric buffer to the HDR scene.
//
// The blend does the work: the buffer holds scattered light already multiplied by its
// own coverage, and alpha is how much of the background the medium hides. With
// premultiplied blending that composites in one step as
//
//   hdr * transmittance + inscatter
//
// which is exactly the lerp toward fog colour the analytic path applied, with the
// terms measured rather than assumed.
//
// The upsample is depth-aware. A plain bilinear fetch averages fog computed for a
// distant background into pixels belonging to a near object, and that shows up as a
// bright halo tracing every silhouette - the classic tell of a half-res volumetric.
// Weighting each of the four taps by how well its depth matches this pixel's keeps the
// fog on the side of the edge it was computed for.

[[vk::binding(0, 0)]] Texture2D    g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

#include "include/FrameConstants.slangh"
#include "include/FullScreenTriangle.slangh"

struct VolumetricCompositePush
{
	uint64_t frameConstantsAddr;
	uint     volumetricSlot;
	uint     depthSlot;
	uint     halfWidth;
	uint     halfHeight;
	uint     _pad0;
	uint     _pad1;
};
[[vk::push_constant]] VolumetricCompositePush pc;

// Linear view depth, so the tap comparison is in metres rather than in the depth
// buffer's heavily non-linear units, where every distant pair looks identical.
float LinearizeDepth(float ndcDepth, FrameConstantsData* fc, float2 uv)
{
	const float4 clip = float4(uv * 2.0f - 1.0f, ndcDepth, 1.0f);
	const float4 world = mul(fc->invViewProj, clip);
	return distance(world.xyz / max(world.w, 1e-6f), fc->cameraWorldPos.xyz);
}

[shader("fragment")]
float4 fragmentMain(float4 svPos : SV_Position) : SV_Target0
{
	FrameConstantsData* fc = (FrameConstantsData*)pc.frameConstantsAddr;

	if (fc->skyParams.z <= 0.0f || fc->fogParams.x <= 0.0f)
	{
		// Nothing scattered and nothing hidden: the blend leaves the scene untouched.
		return float4(0.0f, 0.0f, 0.0f, 0.0f);
	}

	const int2 coord = int2(svPos.xy);
	const int2 half0 = coord / 2;

	const float2 fullUv = (float2(coord) + 0.5f) / float2(float(pc.halfWidth) * 2.0f, float(pc.halfHeight) * 2.0f);
	const float centreDepth = g_textures[pc.depthSlot].Load(int3(coord, 0)).r;
	const float centreLinear = (centreDepth < 1.0f) ? LinearizeDepth(centreDepth, fc, fullUv) : 1e6f;

	float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
	float weightSum = 0.0f;

	[unroll]
	for (int oy = 0; oy < 2; ++oy)
	{
		[unroll]
		for (int ox = 0; ox < 2; ++ox)
		{
			const int2 tap = clamp(half0 + int2(ox, oy), int2(0, 0), int2(int(pc.halfWidth) - 1, int(pc.halfHeight) - 1));

			// The full-res texel this half-res tap was marched from, so its depth is
			// compared against the one it actually used.
			const int2 tapFull = tap * 2;
			const float2 tapUv = (float2(tapFull) + 0.5f) / float2(float(pc.halfWidth) * 2.0f, float(pc.halfHeight) * 2.0f);
			const float tapDepth = g_textures[pc.depthSlot].Load(int3(tapFull, 0)).r;
			const float tapLinear = (tapDepth < 1.0f) ? LinearizeDepth(tapDepth, fc, tapUv) : 1e6f;

			// Tolerance grows with distance: a metre of disagreement is a different
			// surface up close and the same one far away.
			const float tolerance = max(centreLinear * 0.05f, 0.25f);
			const float w = 1.0f / (1.0f + abs(tapLinear - centreLinear) / tolerance);

			sum += g_textures[pc.volumetricSlot].Load(int3(tap, 0)) * w;
			weightSum += w;
		}
	}

	return sum / max(weightSum, 1e-4f);
}
