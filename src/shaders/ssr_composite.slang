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
// Two approximations remain, both second-order and both in the SUBTRACTION only:
//   * F0 is scalar, interpolated from 0.04 to 1 by metallic, where a real metal
//     tints it by base colour. Base colour is not in the G-buffer. Dielectrics -
//     floors, which is where this is most visible - cancel exactly.
//   * Ambient occlusion and fog modulate the forward pass's ambient after this
//     term, and are not reapplied here.

[[vk::binding(0, 0)]] Texture2D    g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

#include "include/FrameConstants.slangh"
#include "include/FullScreenTriangle.slangh"
#include "include/OctNormal.slangh"
#include "include/Sky.slangh"

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
	const float4 ssr = g_textures[pc.ssrSlot].Load(int3(coord, 0));
	const float confidence = ssr.a;
	if (confidence <= 0.001f)
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
		return float4(ssr.rgb, 1.0f);
	}

	const float depth = g_textures[uint(depthEntry.address)].Load(int3(coord, 0)).r;
	if (depth >= 1.0f)
	{
		return float4(ssr.rgb, 1.0f);
	}

	const float4 gbuf = g_textures[uint(gbufEntry.address)].Load(int3(coord, 0));
	const float3 N = DecodeOctNormal(gbuf.rg);
	const float roughness = gbuf.b;
	const float metallic = gbuf.a;

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
	const float3 skySpecular = SkyGradient(skySpecularDir, sunDir, fc->sunColor.rgb, sunIntensity, fc->skyHorizonColor.rgb, fc->skyZenithColor.rgb, fc->skyVoidColor.rgb);

	// Karis' analytic split-sum fit, matching the forward shader.
	const float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
	const float4 c1 = float4(1.0f, 0.0425f, 1.04f, -0.04f);
	const float4 r = roughness * c0 + c1;
	const float a004 = min(r.x * r.x, exp2(-9.28f * NdotV)) * r.x + r.y;
	const float2 envBrdf = float2(-1.04f, 1.04f) * a004 + r.zw;

	const float F0 = lerp(0.04f, 1.0f, metallic);
	const float specWeight = F0 * envBrdf.x + envBrdf.y;

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
