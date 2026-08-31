// Blends the screen-space reflection buffer over the scene.
//
// Separate from the march because that pass reads the HDR colour to fetch what a
// ray landed on, and a pass cannot sample the target it is writing.
//
// The subtlety is that the forward pass has already put a reflection on this
// surface: the analytic sky probe, which assumes every surface reflects open air.
// Adding the ray's result on top of that counts the reflection twice, and the
// surface reads hot exactly where the reflection is strongest.
//
// The usual fix is for the forward pass to output its specular to its own target
// so this pass can replace rather than add. That costs a full extra render target
// and a contract change for every shader drawing into the forward pass. It is not
// needed here, because the sky is analytic: this pass has the normal, roughness
// and metallic from the G-buffer and the same sky function the forward shader
// used, so it can recompute precisely what was added and take it back out.
//
// So the contribution is (reflection - sky) * confidence, added. Where a ray hit
// with full confidence the sky term cancels and the reflection replaces it; where
// it missed, confidence is zero and nothing changes.
//
// The subtraction reproduces what the forward pass added term for term: the same
// sky function, the same split-sum fit, the same specular occlusion and fog, and
// the same COLOURED F0 - a metal reflects its own base colour, so the prepass
// writes base colour into a second target for exactly this.

[[vk::binding(0, 0)]] Texture2D    g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

#include "include/FrameConstants.slangh"
#include "include/FullScreenTriangle.slangh"
#include "include/OctNormal.slangh"
#include "include/EnvBrdf.slangh"
#include "include/Sky.slangh"
#include "include/HeightFog.slangh"
#include "include/ShadowSampling.slangh"

struct SsrCompositePush
{
	uint64_t frameConstantsAddr;
	uint     ssrSlot;
	uint     width;
	uint     height;
	uint     _pad0;
};
[[vk::push_constant]] SsrCompositePush pc;

[shader("fragment")]
float4 fragmentMain(float4 svPos : SV_Position) : SV_Target0
{
	const int2 coord = int2(svPos.xy);

	// Peek at the centre first: most of the screen has no reflection at all, and
	// there is no point gathering a disk to average a set of zeroes.
	const float4 centreSsr = g_textures[pc.ssrSlot].Load(int3(coord, 0));
	if (centreSsr.a <= 0.001f)
	{
		return float4(0.0f, 0.0f, 0.0f, 1.0f);
	}

	FrameConstantsData* fc = (FrameConstantsData*)pc.frameConstantsAddr;

	const ResourceEntry depthEntry = LoadResourceEntry(fc->resourceTableAddr, kFrameResourceId_SceneDepth);
	const ResourceEntry gbufEntry = LoadResourceEntry(fc->resourceTableAddr, kFrameResourceId_SceneGBuffer);
	if (depthEntry.type != kResourceTypeBindlessTexture || gbufEntry.type != kResourceTypeBindlessTexture)
	{
		// No way to work out what the sky put here, so fall back to adding. Too
		// bright is a better failure than a reflection that vanishes.
		return float4(centreSsr.rgb, 1.0f);
	}

	const float depth = g_textures[uint(depthEntry.address)].Load(int3(coord, 0)).r;
	if (depth >= 1.0f)
	{
		return float4(centreSsr.rgb, 1.0f);
	}

	const float4 gbuf = g_textures[uint(gbufEntry.address)].Load(int3(coord, 0));
	const float3 N = DecodeOctNormal(gbuf.rg);
	const float roughness = gbuf.b;
	const float metallic = gbuf.a;

	// One ray gives a mirror. A rough surface reflects a lobe, so the sharp result
	// has to be spread to match: gather a disk whose radius grows with roughness.
	// Blurring the reflection afterwards is what a prefiltered probe would have
	// given directly from a coarser mip, and it costs a handful of taps instead of
	// a ray per sample of the lobe.
	//
	// Premultiplied throughout, so colour and confidence average together and a tap
	// that found nothing correctly dilutes its neighbours rather than darkening
	// them - averaging unpremultiplied colour would let a miss wash out a hit.
	float4 ssr = centreSsr;
	if (roughness > 0.03f)
	{
		const float radiusPx = roughness * 24.0f;
		const float phi = InterleavedGradientNoise(svPos.xy) * kTwoPi;
		float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
		[unroll]
		for (uint i = 0u; i < 8u; ++i)
		{
			const float2 o = VogelDiskSample(i, 8u, phi) * radiusPx;
			const int2 t = clamp(coord + int2(o), int2(0, 0), int2(int(pc.width) - 1, int(pc.height) - 1));
			sum += g_textures[pc.ssrSlot].Load(int3(t, 0));
		}
		ssr = sum * (1.0f / 8.0f);
	}
	const float confidence = ssr.a;

	const float2 uv = (float2(coord) + 0.5f) / float2(pc.width, pc.height);
	const float4 clip = float4(uv * 2.0f - 1.0f, depth, 1.0f);
	const float4 worldH = mul(fc->invViewProj, clip);
	const float3 worldPos = worldH.xyz / max(worldH.w, 1e-6f);

	const float3 V = normalize(fc->cameraWorldPos.xyz - worldPos);
	const float NdotV = max(dot(N, V), 0.0f);
	const float3 R = reflect(-V, N);

	// Exactly the probe direction the forward shader used, so the term cancels
	// rather than merely resembling what was added.
	const float3 sunDir = normalize(fc->sunDirectionIntensity.xyz);
	const float sunIntensity = fc->sunDirectionIntensity.w;
	const float3 skySpecularDir = normalize(lerp(R, N, roughness * roughness));
	const float3 skySpecular = SkyGradient(skySpecularDir, sunDir, fc->sunColor.rgb, sunIntensity, fc->skyHorizonColor.rgb, fc->skyZenithColor.rgb, fc->skyVoidColor.rgb, fc->skyParams, fc->shadingParams.y);

	// Shared with the forward shader, so the term this subtracts cannot drift from the
	// term that was added.
	const float2 envBrdf = EnvBrdfApprox(roughness, NdotV);

	// A metal's Fresnel reflectance is its base colour - gold reflects gold - so this
	// has to be the same float3 the forward pass used, not a scalar standing in for it.
	// With a scalar every channel where the metal is dark was over-subtracted; on copper
	// that removed about half the blue the sky had put there, and the reflection clipped
	// to black.
	//
	// Rebuilt from base colour rather than read as F0 directly, so metallic == 0 gives
	// exactly 0.04 and dielectrics keep cancelling exactly - quantising 0.04 into a
	// texel would have broken that everywhere it is most visible.
	float3 tint = float3(1.0f, 1.0f, 1.0f);
	const ResourceEntry baseEntry = LoadResourceEntry(fc->resourceTableAddr, kFrameResourceId_SceneBaseColor);
	if (baseEntry.type == kResourceTypeBindlessTexture)
	{
		tint = g_textures[uint(baseEntry.address)].Load(int3(coord, 0)).rgb;
	}
	const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), tint, metallic);
	float3 specWeight = EnvSpecularWeight(F0, envBrdf);

	// The forward pass did not add the sky specular raw - it multiplied it by
	// specular occlusion and then fogged the result. Subtracting the raw value would
	// take out more than was ever put in, darkening exactly the creases and hazy
	// distances where the sky contributed least. Both factors scale the whole
	// contribution, because the reflection replacing that sky is occluded and fogged
	// by the same amounts.
	{
		float ao = 1.0f;
		ResourceEntry aoEntry = LoadResourceEntry(fc->resourceTableAddr, kFrameResourceId_Gtao);
		if (aoEntry.type == kResourceTypeBindlessTexture && aoEntry.width > 0u && aoEntry.height > 0u)
		{
			ao = saturate(g_textures[uint(aoEntry.address)].SampleLevel(g_linearSampler, uv, 0.0f).r);
		}
		// Lagarde's specular occlusion, matching the forward shader exactly.
		const float specularAo = saturate(pow(NdotV + ao, exp2(-16.0f * roughness - 1.0f)) - 1.0f + ao);
		specWeight *= specularAo;
	}

	// Zero while the volumetric pass is running: it fogs the composited result
	// afterwards, so taking it out here as well would apply the air twice.
	const float fogAmount = (fc->skyParams.z > 0.0f)
	        ? 0.0f
	        : HeightFogAmount(fc->cameraWorldPos.xyz, worldPos, fc->fogParams.x, fc->fogParams.y, fc->fogParams.w);
	specWeight *= (1.0f - fogAmount);

	// The march returns the raw colour it landed on. A surface does not reflect all
	// of that: it reflects the Fresnel-weighted fraction, which for a dielectric is
	// a few percent. Weighting BOTH terms by specWeight is what makes the sky term
	// cancel and keeps the reflection at the strength the BRDF actually calls for -
	// adding the unweighted colour was making every reflection far too strong.
	//
	// ssr.rgb is already premultiplied by confidence, so the sky is weighted by it
	// too and the two cancel exactly where a ray hit with full confidence.
	const float3 contribution = (ssr.rgb - skySpecular * confidence) * specWeight;
	return float4(contribution, 1.0f);
}
