// Volumetric fog: the height fog integral solved by marching, so shadows can carve
// beams out of it.
//
// HeightFog.slangh already solves the same integral in closed form - light scattered
// toward the eye, plus what survived the air - and does it exactly, for free. What a
// closed form cannot know is which parts of the air the sun actually reaches. Air only
// reads as a volume once something can block it; that is the whole difference between
// haze and a shaft of light.
//
// So this is not a second fog model sitting beside the first. It is the same one,
// term for term:
//
//   analytic:  colour = ambientScatter + sunLobe
//   marched:   sum over steps of T * sigma * dt * (ambientScatter + sunLobe * shadow)
//
// With nothing shadowed the march reproduces the analytic result, which is why turning
// this on does not restyle a scene - it only subtracts light that was never there.
// That equivalence is also the first thing to check if it ever looks wrong.
//
// The forward pass and the reflection composite skip their analytic fog whenever this
// is running, or the same scattering would be counted twice.
//
// Half resolution, because the quantity is smooth: an integral along a ray through a
// medium has no high-frequency detail of its own. The only edges it carries are the
// depth buffer's, and those are recovered on upsample rather than marched for.

[[vk::binding(0, 0)]] Texture2D    g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

#include "include/FrameConstants.slangh"
#include "include/FullScreenTriangle.slangh"
#include "include/ShadowSampling.slangh"
#include "include/DirectionalShadowCSM.slangh"
#include "include/Sky.slangh"

struct VolumetricPush
{
	uint64_t frameConstantsAddr;
	uint     width;      // half-res target
	uint     height;
	uint     depthSlot;  // full-res scene depth
	uint     _pad0;
};
[[vk::push_constant]] VolumetricPush pc;

// Enough to keep banding under the jitter. More steps buy very little on a medium this
// smooth and cost a shadow lookup each.
static const uint kSteps = 32u;

// Sun visibility for a point in mid-air.
//
// Deliberately not ComputeDirectionalShadow: that one filters a penumbra and offsets
// its lookup along a surface normal, and a point in a medium has no normal to offset
// along. It is also called 32 times per pixel here, where a blocker search would cost
// more than the rest of the pass put together. One comparison per step, with the
// march's own jitter standing in for a filter.
float SunVisibilityInAir(FrameConstantsData* fc, float3 worldPos)
{
	const float viewDepth = max(-mul(fc->view, float4(worldPos, 1.0f)).z, 0.0f);

	uint cascade = 0u;
	if (viewDepth > fc->shadowCascadeSplits.x) { cascade = 1u; }
	if (viewDepth > fc->shadowCascadeSplits.y) { cascade = 2u; }

	const ResourceEntry entry = LoadResourceEntry(fc->resourceTableAddr, kFrameResourceId_DirectionalShadowC0 + cascade);
	const uint slot = uint(entry.address);
	if (entry.type != kResourceTypeBindlessTexture || slot == 0xFFFFFFFFu || entry.width == 0u || entry.height == 0u)
	{
		return 1.0f;
	}

	const float4 clip = mul(fc->shadowViewProjCascades[cascade], float4(worldPos, 1.0f));
	if (abs(clip.w) < 1e-6f)
	{
		return 1.0f;
	}
	const float3 ndc = clip.xyz / clip.w;
	const float2 uv = ndc.xy * 0.5f + 0.5f;
	// Outside this cascade, or past its depth range: unlit air would be a black slab
	// with a hard edge, so the honest answer is "no information here, assume lit".
	if (uv.x <= 0.0f || uv.x >= 1.0f || uv.y <= 0.0f || uv.y >= 1.0f || ndc.z <= 0.0f || ndc.z >= 1.0f)
	{
		return 1.0f;
	}

	const float d = LoadCascadeDepth(slot, uv, entry.width, entry.height);
	return ((saturate(ndc.z) - fc->shadowCascadeDepthBias[cascade]) <= d) ? 1.0f : 0.0f;
}

[shader("fragment")]
float4 fragmentMain(float4 svPos : SV_Position) : SV_Target0
{
	FrameConstantsData* fc = (FrameConstantsData*)pc.frameConstantsAddr;

	const float density = fc->fogParams.x;
	const float strength = fc->skyParams.z;
	if (density <= 0.0f || strength <= 0.0f)
	{
		return float4(0.0f, 0.0f, 0.0f, 0.0f);
	}

	const int2 coord = int2(svPos.xy);
	const float2 uv = (float2(coord) + 0.5f) / float2(pc.width, pc.height);

	// Point-sample the matching full-res depth rather than averaging the 2x2. An
	// averaged depth belongs to no surface, and marching to it puts fog slightly in
	// front of or behind every silhouette.
	const int2 fullCoord = coord * 2;
	const float sceneDepth = g_textures[pc.depthSlot].Load(int3(fullCoord, 0)).r;

	// World-space ray for this pixel.
	const float2 ndcXy = uv * 2.0f - 1.0f;
	const float4 nearClip = mul(fc->invViewProj, float4(ndcXy, 0.0f, 1.0f));
	const float4 farClip = mul(fc->invViewProj, float4(ndcXy, 1.0f, 1.0f));
	const float3 nearPos = nearClip.xyz / max(nearClip.w, 1e-6f);
	const float3 farPos = farClip.xyz / max(farClip.w, 1e-6f);
	const float3 rayDir = normalize(farPos - nearPos);

	const float3 camPos = fc->cameraWorldPos.xyz;

	// Sky pixels are left alone, exactly as the analytic path leaves them alone: there
	// is no surface, so there is no segment of air between the eye and anything. The
	// sky already carries its own scattering - it IS scattered light - and hazing it a
	// second time washes the whole vault out, which is what an earlier version of this
	// pass did.
	//
	// The cost is that shafts do not show against open sky, only against geometry.
	// Getting those means deciding how far "the sky" is and reconciling that with the
	// atmosphere already in it, which is a separate problem from this one.
	if (sceneDepth >= 1.0f)
	{
		return float4(0.0f, 0.0f, 0.0f, 0.0f);
	}

	// Past the last cascade every step answers "lit" anyway, so marching further buys
	// nothing but samples. Taken from the cascade config rather than pushed, so it
	// cannot drift out of step with the shadows it is sampling.
	const float maxDistance = max(fc->shadowCascadeSplits.z, 1.0f);

	const float4 surfClip = mul(fc->invViewProj, float4(ndcXy, sceneDepth, 1.0f));
	const float3 surfPos = surfClip.xyz / max(surfClip.w, 1e-6f);
	const float marchLength = min(distance(surfPos, camPos), maxDistance);
	if (marchLength <= 1e-3f)
	{
		return float4(0.0f, 0.0f, 0.0f, 0.0f);
	}

	const float3 sunDir = normalize(fc->sunDirectionIntensity.xyz);
	const float sunIntensity = fc->sunDirectionIntensity.w;

	// Both scattering terms are constant along the ray - the view direction does not
	// change - so they are evaluated once and only the shadow varies per step.
	const float3 ambientScatter = SkyGradient(rayDir, sunDir, fc->sunColor.rgb, sunIntensity, fc->skyHorizonColor.rgb, fc->skyZenithColor.rgb, fc->skyVoidColor.rgb, fc->skyParams);

	const float g = clamp(fc->skyParams.w, 0.0f, 0.95f);
	const float cosTheta = dot(rayDir, sunDir);
	const float denom = 1.0f + g * g - 2.0f * g * cosTheta;
	const float phase = (1.0f - g * g) / (4.0f * 3.14159265f * max(denom * sqrt(max(denom, 1e-4f)), 1e-4f));
	const float sunAboveHorizon = saturate(sunDir.y * 4.0f);
	const float3 sunLobe = fc->sunColor.rgb * sunIntensity * phase * fc->fogParams.z * sunAboveHorizon;

	const float k = max(fc->fogParams.y, 1e-4f);
	const float dt = marchLength / float(kSteps);

	// Jitter the first step. A fixed start puts every pixel's samples on the same
	// planes, and those planes are visible as banding wherever the medium is thick.
	// Interleaved gradient noise is stable frame to frame, so this reads as grain
	// rather than as crawl.
	const float jitter = InterleavedGradientNoise(svPos.xy);

	float3 inscatter = float3(0.0f, 0.0f, 0.0f);
	float transmittance = 1.0f;

	[loop]
	for (uint i = 0u; i < kSteps; ++i)
	{
		const float t = (float(i) + jitter) * dt;
		const float3 p = camPos + rayDir * t;

		// The same exponential height falloff the closed form integrates, sampled.
		const float sigma = density * exp2(-k * p.y * 1.442695f);
		if (sigma <= 1e-6f)
		{
			continue;
		}

		const float shadow = SunVisibilityInAir(fc, p);
		const float3 scattered = ambientScatter + sunLobe * shadow;

		// Accumulate the step before attenuating, so the near end of the ray is not
		// dimmed by air it has not travelled through yet.
		inscatter += transmittance * sigma * dt * scattered;
		transmittance *= exp2(-sigma * dt * 1.442695f);
	}

	// The same ceiling the analytic fog honours, so the far plane never becomes a flat
	// wall of colour. Both terms are scaled by it, not just the alpha: the composite is
	// a lerp toward the scattered colour, and clamping one side of a lerp while leaving
	// the other at full strength adds light that was never scattered.
	const float reached = 1.0f - transmittance;
	const float opacity = min(reached, fc->fogParams.w);
	const float clampScale = (reached > 1e-5f) ? (opacity / reached) : 0.0f;
	return float4(inscatter * clampScale * strength, opacity);
}
