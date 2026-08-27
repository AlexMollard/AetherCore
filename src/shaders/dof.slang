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

	float3 sum = g_textures[pc.hdrSlot].SampleLevel(g_linearSampler, uv, 0.0f).rgb;
	float weight = 1.0f;

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
			const float tapRadius = abs(CircleOfConfusion(tapDepth, tapUv, fc, fullSize.y)) * 0.25f;

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
			sum += g_textures[pc.hdrSlot].SampleLevel(g_linearSampler, tapUv, 0.0f).rgb * w;
			weight += w;
		}
	}

	// Alpha is how far this pixel has left focus: nothing until the disc is wider than
	// the pixel it is drawn on, because below that a lens is sharp.
	const float blend = saturate(abs(centreCoc) - 1.0f);
	return float4(sum / max(weight, 1e-4f), blend);
}
