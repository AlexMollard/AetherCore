// Depth of field: a lens focuses one plane, and everything else lands as a disc.
//
// The size of that disc is optics, not a stylistic radius. Focal length and aperture are
// authored on the camera, AetherCore.cpp folds the constant part into one coefficient,
// and all that is left here is |d - S| / d.
//
// Gathering, not scattering. A lens really does SPREAD each point over its disc, which a
// rasteriser cannot do directly - so instead each pixel asks its neighbours "is your disc
// wide enough to reach me?" and averages the ones that say yes. That test is what keeps a
// sharp foreground from smearing into a blurred background: a sharp pixel's disc is a
// point, so it never reaches anywhere it should not, however blurred its neighbour is.
//
// Half resolution, because the output is by definition out of focus. There is no detail
// in it to lose, and the taps get a free 2x2 average from sampling the full-res colour
// bilinearly.

[[vk::binding(0, 0)]] Texture2D    g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

#include "include/FrameConstants.slangh"
#include "include/FullScreenTriangle.slangh"
#include "include/ShadowSampling.slangh"

struct DofPush
{
	uint64_t frameConstantsAddr;
	uint     hdrSlot;
	uint     depthSlot;
	uint     width;   // half-res target
	uint     height;
	float    focusDistance;
	float    cocCoeff;
	float    maxCocPixels;
};
[[vk::push_constant]] DofPush pc;

// Enough of the disc to read as bokeh rather than a ring of copies. A Vogel spiral
// spreads them evenly at any count, so this is a straight cost dial.
static const uint kTaps = 24u;

float ViewDistance(float ndcDepth, float2 uv, FrameConstantsData* fc)
{
	const float4 clip = float4(uv * 2.0f - 1.0f, ndcDepth, 1.0f);
	const float4 world = mul(fc->invViewProj, clip);
	return distance(world.xyz / max(world.w, 1e-6f), fc->cameraWorldPos.xyz);
}

// Blur circle DIAMETER in full-resolution pixels, signed: negative in front of the focus
// plane, positive behind it. The sign only tells near from far; the disc uses the size.
float CircleOfConfusion(float ndcDepth, float2 uv, FrameConstantsData* fc, float fullHeight)
{
	if (ndcDepth >= 1.0f)
	{
		// Sky sits at infinity, which is as far out of focus as anything gets.
		return pc.maxCocPixels;
	}
	const float d = ViewDistance(ndcDepth, uv, fc);
	const float signedGap = d - pc.focusDistance;
	const float coc = pc.cocCoeff * fullHeight * abs(signedGap) / max(d, 1e-4f);
	return clamp(coc, 0.0f, pc.maxCocPixels) * ((signedGap < 0.0f) ? -1.0f : 1.0f);
}

[shader("fragment")]
float4 fragmentMain(float4 svPos : SV_Position) : SV_Target0
{
	FrameConstantsData* fc = (FrameConstantsData*)pc.frameConstantsAddr;

	const int2 coord = int2(svPos.xy);
	const float2 halfSize = float2(pc.width, pc.height);
	const float2 fullSize = halfSize * 2.0f;
	const float2 uv = (float2(coord) + 0.5f) / halfSize;

	const int2 fullCoord = coord * 2;
	const float centreDepth = g_textures[pc.depthSlot].Load(int3(fullCoord, 0)).r;
	const float centreCoc = CircleOfConfusion(centreDepth, uv, fc, fullSize.y);

	// Radius in HALF-res pixels: the value is a full-res diameter, so halve it for a
	// radius and halve it again for the resolution.
	const float centreRadius = abs(centreCoc) * 0.25f;

	// Near and far are gathered SEPARATELY, because they occlude differently.
	//
	// Something behind the focus plane is hidden by whatever is in front of it, so its
	// blur has to stay behind the silhouette that covers it. Something in FRONT of the
	// focus plane is not hidden by anything - the lens spreads it over the background,
	// and its own outline dissolves. A single accumulator cannot express both: with one
	// pool the output alpha can only describe how defocused THIS pixel is, so a sharp
	// background pixel under a blurred foreground reports alpha 0 and keeps its sharp
	// value. The foreground then stops dead at its own silhouette - a hard edge on the
	// one part of the image that should have none.
	//
	// A tap is near field only if it is both in front of focus and actually defocused;
	// a sharp near tap belongs with the far pool, where its own outline is preserved.
	const bool centreIsNear = (centreCoc < 0.0f) && (centreRadius > 0.5f);

	// The centre is a tap like any other and must carry the same weight, or it wins by
	// default. A tap's weight is 1/r^2 because a wide disc spreads the same light over
	// more area; giving the centre a flat 1.0 while its neighbours each got 1/36 left a
	// fully defocused pixel still 60% its own sharp value, and the silhouette of a sphere
	// well outside the focus plane softened by two pixels instead of twenty-four.
	const float centreWeight = 1.0f / max(centreRadius * centreRadius, 1.0f);
	const float3 centreColour = g_textures[pc.hdrSlot].SampleLevel(g_linearSampler, uv, 0.0f).rgb;

	float3 farSum = centreIsNear ? float3(0.0f, 0.0f, 0.0f) : centreColour * centreWeight;
	float farWeight = centreIsNear ? 0.0f : centreWeight;
	float3 nearSum = centreIsNear ? centreColour * centreWeight : float3(0.0f, 0.0f, 0.0f);
	float nearWeight = centreIsNear ? centreWeight : 0.0f;

	// Accumulated only over the spiral, never the centre: the coverage estimate below
	// assumes a uniform sample density over the search disc, and the centre is an extra
	// sample sitting at the origin rather than part of that set.
	float nearCoverageWeight = 0.0f;

	// The gather radius has to cover the widest disc that could reach here, not this
	// pixel's own. A sharp pixel in front of a blurred background still needs to collect
	// from it, and its own radius is zero.
	const float searchRadius = max(centreRadius, pc.maxCocPixels * 0.25f);
	if (searchRadius > 0.5f)
	{
		// Rotate each pixel's spiral so the taps do not land on the same angles
		// everywhere, which would show the disc as a fixed star pattern.
		const float phi = InterleavedGradientNoise(svPos.xy) * kTwoPi;

		[unroll]
		for (uint i = 0u; i < kTaps; ++i)
		{
			const float2 offset = VogelDiskSample(i, kTaps, phi) * searchRadius;
			const float dist = length(offset);
			const float2 tapUv = clamp((float2(coord) + 0.5f + offset) / halfSize, 0.0f, 1.0f);

			const int2 tapFull = clamp(int2(tapUv * fullSize), int2(0, 0), int2(int(fullSize.x) - 1, int(fullSize.y) - 1));
			const float tapDepth = g_textures[pc.depthSlot].Load(int3(tapFull, 0)).r;
			const float tapCoc = CircleOfConfusion(tapDepth, tapUv, fc, fullSize.y);
			const float tapRadius = abs(tapCoc) * 0.25f;

			// The whole test: does this neighbour's disc actually cover this pixel? A
			// half-texel of slack keeps the transition from quantising into rings.
			if (tapRadius + 0.5f < dist)
			{
				continue;
			}

			// A wide disc spreads the same light over more area, so it arrives dimmer.
			// Without this a single bright out-of-focus pixel dominates everything its
			// disc touches and reads as a hard blob rather than as bokeh.
			const float w = 1.0f / max(tapRadius * tapRadius, 1.0f);
			const float3 tapColour = g_textures[pc.hdrSlot].SampleLevel(g_linearSampler, tapUv, 0.0f).rgb;

			if (tapCoc < 0.0f && tapRadius > 0.5f)
			{
				nearSum += tapColour * w;
				nearWeight += w;
				nearCoverageWeight += w;
			}
			else
			{
				farSum += tapColour * w;
				farWeight += w;
			}
		}
	}

	// The far pool can legitimately come up EMPTY. Inside a near silhouette every
	// background tap is sharp, and a sharp tap's disc is a point - so it correctly fails
	// the reach test and contributes nothing. Dividing by that empty weight yields black,
	// and compositing the near field over black darkens the inside of every near edge:
	// measured as a ring dipping to 104 against a 117 backdrop, a dark halo exactly where
	// the silhouette should be dissolving.
	//
	// What is behind the near object there is genuinely unknown - it is occluded, and a
	// gather cannot invent it. The centre's own colour is the one honest estimate of what
	// this pixel shows, so the near field blends toward that instead of toward nothing.
	// Outside the silhouette the centre is itself a far sample, so farWeight is never zero
	// there and this changes nothing.
	const float3 farColour = (farWeight > 1e-4f) ? (farSum / farWeight) : centreColour;
	const float3 nearColour = (nearWeight > 1e-4f) ? (nearSum / nearWeight) : centreColour;

	// How much of the lens the near field blocks, derived rather than dialled in.
	//
	// The taps are spread uniformly over the search disc, so each one stands for an area
	// of pi*R^2/kTaps. A near tap of radius r was itself smeared across pi*r^2 by the
	// lens, so the share of the aperture looking at it is the ratio of those two areas,
	// R^2 / (kTaps * r^2) - which is the 1/r^2 weight already in hand, scaled by
	// R^2/kTaps. Summing over the near taps gives the blocked fraction directly.
	//
	// It lands where it should at both ends: a pixel entirely inside a near object whose
	// blur fills the search disc collects kTaps taps of weight 1/R^2 and comes out at
	// exactly 1, and a small near object that fills only its own narrower disc still
	// reaches 1 because its taps are correspondingly heavier. Nothing to tune.
	const float nearCoverage = saturate(nearCoverageWeight * (searchRadius * searchRadius) / float(kTaps));

	// Alpha is how far this pixel has left focus: nothing until the disc is wider than
	// the pixel it is drawn on, because below that a lens is sharp. A pixel that is
	// itself sharp still has to yield to a near disc lying over it, so whichever of the
	// two claims more wins.
	const float farBlend = saturate(abs(centreCoc) - 1.0f);
	const float alpha = max(farBlend, nearCoverage);

	// The colour has to be what belongs at THIS alpha, not the finished pixel.
	//
	// The tonemap composites lerp(sharp, thisColour, alpha), so a near edge carrying
	// coverage c would otherwise land twice: once mixing the near field in at c here,
	// and again as the alpha the blend applies - leaving c^2 of it. At 35% coverage that
	// is 12%, and it was measured: the orange bled about a third as far past the
	// silhouette as its circle of confusion says it should.
	//
	// So each field is weighted by the share of the blend it is entitled to and the sum
	// divided back out by alpha, which the composite then multiplies straight back in.
	// Every case falls out of it: pure far gives farColour at alpha 1, pure near gives
	// nearColour at alpha 1, and a near edge over a sharp background gives nearColour at
	// alpha c - so the composite mixes it exactly once, at c.
	const float3 colour = (farColour * farBlend * (1.0f - nearCoverage) + nearColour * nearCoverage) / max(alpha, 1e-4f);

	return float4(colour, alpha);
}
