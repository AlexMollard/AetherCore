// Screen-space reflections.
//
// The sky probe in the forward shader answers "what does this surface reflect?"
// with the sky, always. That is right for a surface facing open air and wrong for
// every surface with something in front of it: a floor reflects the objects
// standing on it, not the clouds above them. The depth buffer already holds those
// objects, so marching a ray through it recovers the reflection that is actually
// there.
//
// What this can and cannot do is worth being explicit about, because the failures
// are structural rather than bugs:
//   * Only geometry that is on screen can be reflected. A ray leaving the frame
//     has no information, so it fades out rather than inventing one.
//   * Only the front-most surface exists in a depth buffer. A ray passing behind
//     an object cannot know how thick that object is, so a thickness test decides
//     whether a crossing is a hit or the ray simply travelling behind something.
// Both are why this composites over the sky probe rather than replacing it: where
// the ray fails, the analytic sky is the fallback, and the transition is a fade.
//
// The march is in screen space at a fixed pixel stride, not in world space. A
// world-space step projects to wildly different pixel distances depending on
// depth - fine steps wasted in the distance, gaps up close.

[[vk::binding(0, 0)]] Texture2D    g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

#include "include/FrameConstants.slangh"
#include "include/FullScreenTriangle.slangh"
#include "include/OctNormal.slangh"
#include "include/ShadowSampling.slangh"

struct SsrPush
{
	uint64_t frameConstantsAddr;
	uint     width;
	uint     height;
	float    maxRoughness;   // above this the lobe is too wide for a single ray
	float    intensity;
	uint     _pad0;
	uint     _pad1;
};
[[vk::push_constant]] SsrPush pc;

// Steps along the ray. Screen-space, so this is a real pixel budget rather than a
// distance that means different things at different depths.
static const uint  kMaxSteps = 48u;
static const float kStrideBase = 3.0f;

// A crossing counts as a hit only if the ray is within this much of the surface,
// in view-space depth. The depth buffer holds one surface per pixel and says
// nothing about how thick it is, so without this a ray travelling far behind an
// object registers as reflecting off its back.
static const float kThickness = 0.55f;

// Refinement steps once a crossing is bracketed. Binary search converges far
// faster than shortening the stride everywhere, which would cost the whole march.
static const uint kRefineSteps = 5u;

float LinearDepth(float ndcDepth, FrameConstantsData* fc)
{
	// Reverse the projection for this NDC depth at the view axis; only the
	// magnitude matters for the thickness test.
	const float4 clip = float4(0.0f, 0.0f, ndcDepth, 1.0f);
	const float4 view = mul(fc->invViewProj, clip);
	return length(view.xyz / max(view.w, 1e-6f) - fc->cameraWorldPos.xyz);
}

[shader("fragment")]
float4 fragmentMain(float4 svPos : SV_Position) : SV_Target0
{
	FrameConstantsData* fc = (FrameConstantsData*)pc.frameConstantsAddr;
	const int2 coord = int2(svPos.xy);

	const ResourceEntry depthEntry = LoadResourceEntry(fc->resourceTableAddr, kFrameResourceId_SceneDepth);
	const ResourceEntry gbufEntry = LoadResourceEntry(fc->resourceTableAddr, kFrameResourceId_SceneGBuffer);
	const ResourceEntry hdrEntry = LoadResourceEntry(fc->resourceTableAddr, kFrameResourceId_HdrColor);
	if (depthEntry.type != kResourceTypeBindlessTexture || gbufEntry.type != kResourceTypeBindlessTexture || hdrEntry.type != kResourceTypeBindlessTexture)
	{
		return float4(0.0f, 0.0f, 0.0f, 0.0f);
	}

	const uint depthSlot = uint(depthEntry.address);
	const uint gbufSlot = uint(gbufEntry.address);
	const uint hdrSlot = uint(hdrEntry.address);

	const float centreDepth = g_textures[depthSlot].Load(int3(coord, 0)).r;
	if (centreDepth >= 1.0f)
	{
		// Sky. There is nothing here to reflect anything.
		return float4(0.0f, 0.0f, 0.0f, 0.0f);
	}

	const float4 gbuf = g_textures[gbufSlot].Load(int3(coord, 0));
	const float roughness = gbuf.b;
	if (roughness > pc.maxRoughness)
	{
		// A lobe this wide needs many rays to look like anything but noise, and
		// the sky probe is already a reasonable answer for it. Bail rather than
		// spend the march producing a single noisy sample.
		return float4(0.0f, 0.0f, 0.0f, 0.0f);
	}

	const float3 N = DecodeOctNormal(gbuf.rg);

	// Reconstruct this pixel's world position from depth.
	const float2 uv = (float2(coord) + 0.5f) / float2(pc.width, pc.height);
	const float4 clip = float4(uv * 2.0f - 1.0f, centreDepth, 1.0f);
	float4 worldH = mul(fc->invViewProj, clip);
	const float3 worldPos = worldH.xyz / max(worldH.w, 1e-6f);

	const float3 V = normalize(fc->cameraWorldPos.xyz - worldPos);
	const float3 R = reflect(-V, N);

	// Lift the ray off the surface before marching. At a contact - the base of an
	// object standing on a floor, which is exactly where a reflection is most
	// obviously right or wrong - the reflected ray leaves at a grazing angle and
	// travels nearly parallel to the surface it started on. Without an offset its
	// first samples land on that same surface, register as a hit, and the march
	// stops before reaching the object, leaving a band of missing reflection right
	// where the two meet.
	//
	// Scaled by distance from the camera so it stays a roughly constant number of
	// pixels: a fixed world offset is far too small up close and far too large in
	// the distance, where a texel covers more world.
	const float viewDist = distance(worldPos, fc->cameraWorldPos.xyz);
	const float3 rayOrigin = worldPos + N * (viewDist * 0.004f);

	// A ray heading back toward the camera exits the frame immediately and cannot
	// hit anything useful; skip the march rather than spend it discovering that.
	if (dot(R, V) > 0.999f)
	{
		return float4(0.0f, 0.0f, 0.0f, 0.0f);
	}

	// Project the ray's start and a point along it, and step between them in
	// screen space so the stride is measured in pixels.
	const float rayLength = 40.0f;
	const float3 endWorld = rayOrigin + R * rayLength;
	float4 endClip = mul(fc->viewProj, float4(endWorld, 1.0f));
	if (endClip.w <= 0.0f)
	{
		return float4(0.0f, 0.0f, 0.0f, 0.0f);
	}

	const float3 endNdc = endClip.xyz / endClip.w;

	// Project the lifted origin rather than reusing this pixel's coordinates, so the
	// depth the march starts from matches where the ray actually begins.
	float4 startClip = mul(fc->viewProj, float4(rayOrigin, 1.0f));
	if (startClip.w <= 0.0f)
	{
		return float4(0.0f, 0.0f, 0.0f, 0.0f);
	}
	const float3 startNdc = startClip.xyz / startClip.w;
	const float2 startPx = (startNdc.xy * 0.5f + 0.5f) * float2(pc.width, pc.height);
	const float2 endPx = (endNdc.xy * 0.5f + 0.5f) * float2(pc.width, pc.height);

	const float2 deltaPx = endPx - startPx;
	const float pxLength = max(length(deltaPx), 1e-4f);
	const float stride = max(kStrideBase, pxLength / float(kMaxSteps));
	const uint steps = min(kMaxSteps, uint(pxLength / stride));
	if (steps < 2u)
	{
		return float4(0.0f, 0.0f, 0.0f, 0.0f);
	}

	const float2 stepPx = deltaPx / float(steps);
	const float startNdcZ = startNdc.z;
	const float stepZ = (endNdc.z - startNdcZ) / float(steps);

	// Dither the start so the fixed stride does not band. Interleaved gradient
	// noise is stable frame to frame, so this reads as grain rather than crawl.
	const float jitter = InterleavedGradientNoise(svPos.xy);

	float2 hitPx = float2(0.0f, 0.0f);
	bool hit = false;
	float prevT = 0.0f;

	[loop]
	for (uint i = 1u; i <= steps; ++i)
	{
		const float t = float(i) + jitter;
		const float2 samplePx = startPx + stepPx * t;
		if (any(samplePx < 0.0f) || samplePx.x >= float(pc.width) || samplePx.y >= float(pc.height))
		{
			break; // left the frame: no information out here
		}

		const float rayZ = startNdcZ + stepZ * t;
		const float sceneZ = g_textures[depthSlot].Load(int3(int2(samplePx), 0)).r;

		// Reverse-Z is not in use here: a larger NDC z is further away, so the ray
		// is behind the surface when its z exceeds the scene's.
		if (rayZ > sceneZ && sceneZ < 1.0f)
		{
			const float rayLin = LinearDepth(rayZ, fc);
			const float sceneLin = LinearDepth(sceneZ, fc);
			if (rayLin - sceneLin < kThickness)
			{
				// Bracketed between the previous step and this one; bisect.
				float lo = prevT;
				float hi = t;
				[unroll]
				for (uint r = 0u; r < kRefineSteps; ++r)
				{
					const float mid = (lo + hi) * 0.5f;
					const float2 midPx = startPx + stepPx * mid;
					const float midZ = startNdcZ + stepZ * mid;
					const float midScene = g_textures[depthSlot].Load(int3(int2(midPx), 0)).r;
					if (midZ > midScene)
					{
						hi = mid;
					}
					else
					{
						lo = mid;
					}
				}
				hitPx = startPx + stepPx * hi;
				hit = true;
				break;
			}

			// Crossed this surface but landed further behind it than it could plausibly
			// be thick: the ray did not hit it, it went PAST it. Stopping here was
			// throwing away everything beyond, and the pixel fell back to the analytic
			// sky - which stamped the occluder's silhouette into an otherwise correct
			// reflection, as a sphere-shaped hole in the wall behind it.
			//
			// So a rejected crossing is not the end of the march, it is one surface
			// ruled out. What the ray actually reflects may still be further along.
		}
		prevT = t;
	}

	if (!hit)
	{
		return float4(0.0f, 0.0f, 0.0f, 0.0f);
	}

	const float2 hitUv = hitPx / float2(pc.width, pc.height);

	// Fade out where the result stops being trustworthy, so the sky probe takes
	// over smoothly instead of the reflection ending at a visible edge:
	//   * near the frame border, where the ray was about to run out of screen
	//   * as roughness approaches the cutoff, where one ray stops representing
	//     the lobe
	//   * when the reflection points back at the camera, the direction in which
	//     screen-space information is least reliable
	const float2 edge = min(hitUv, 1.0f - hitUv);
	const float edgeFade = saturate(min(edge.x, edge.y) / 0.08f);
	const float roughFade = saturate(1.0f - roughness / max(pc.maxRoughness, 1e-3f));
	const float facingFade = saturate(1.0f - dot(R, V) * 0.5f - 0.25f);

	const float confidence = edgeFade * roughFade * facingFade * pc.intensity;
	if (confidence <= 0.001f)
	{
		return float4(0.0f, 0.0f, 0.0f, 0.0f);
	}

	const float3 reflected = g_textures[hdrSlot].SampleLevel(g_linearSampler, hitUv, 0.0f).rgb;
	return float4(reflected * confidence, confidence);
}
