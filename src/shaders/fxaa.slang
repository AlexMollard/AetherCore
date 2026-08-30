// Post-process pass: FXAA 3.11 (Lottes), quality path.
//
// Applied after tonemapping on the LDR buffer so edge detection works on
// perceptual luminance rather than linear HDR values, which is where the eye
// actually sees the staircase.
//
// This is the full quality algorithm, not the cut-down cross-filter version:
//
//   1. Contrast reject. Most of the screen has no edge in it; bail before
//      touching the eleven extra taps below.
//   2. Edge orientation from the 3x3 luma neighbourhood.
//   3. Endpoint search ALONG the edge, sampling on the boundary between the
//      two rows (or columns) that form it - a bilinear tap at a half-texel
//      offset averages both sides in one fetch, which is what makes the search
//      track the edge rather than one side of it.
//      The steps lengthen as they go (1,1,1,1,1.5,2,2,2,2,4,8), so eleven
//      iterations reach about 25 pixels. Reach is what decides whether a
//      near-horizontal edge resolves as a smooth line or as a dashed one:
//      a search that gives up mid-edge blends part of it and leaves the rest,
//      which reads worse than no antialiasing at all.
//   4. Correct-variation test. The blend is only applied if the luma at the
//      nearer endpoint sits on the opposite side of the local average from the
//      centre pixel. Without this check, thin bright features - wires, text,
//      specular glints - get blended into their background instead of
//      antialiased, which is the "FXAA looks blurry" complaint.
//   5. Sub-pixel term for features too small to have endpoints at all,
//      clamped to 0.75 so a single pixel can never be fully erased.
//
// The final offset lands somewhere inside +-0.5 of a texel perpendicular to
// the edge; one bilinear fetch there reconstructs the coverage exactly.
//
// Binding layout:
//   set 0, binding 0 - bindless combined image samplers
//   set 0, binding 1 - global linear sampler

[[vk::binding(0, 0)]]
Texture2D g_textures[];
[[vk::binding(1, 0)]]
SamplerState g_linearSampler;

#include "include/FullScreenTriangle.slangh"

struct FxaaPush
{
	uint  ldrSlot;
	uint  fxaaEnabled;
	float rcpWidth;
	float rcpHeight;
	float sharpness; // contrast-adaptive sharpening, 0 = off
};
[[vk::push_constant]] FxaaPush pc;

// Contrast thresholds. Below the absolute one the region is too dark for an
// edge to be visible; below the relative one it is too flat to be an edge.
static const float kAbsoluteThreshold = 0.0833f;
static const float kRelativeThreshold = 0.125f;

// Ceiling on the sub-pixel term. At 1.0 a lone bright pixel is replaced by its
// neighbourhood average and disappears entirely.
static const float kSubpixelQuality = 0.75f;

// Step multipliers for the endpoint search. Fine near the centre where the
// endpoint usually is, coarse further out where being a texel off costs
// nothing, so the reach grows far faster than the iteration count.
static const uint  kSearchSteps = 11u;
static const float kSearchQuality[kSearchSteps] = {
	1.0f, 1.0f, 1.0f, 1.0f, 1.5f, 2.0f, 2.0f, 2.0f, 2.0f, 4.0f, 8.0f
};

float Luma(float3 rgb)
{
	return dot(rgb, float3(0.2126f, 0.7152f, 0.0722f));
}

// The global sampler addresses REPEAT, so a tap past an edge would wrap to the
// far side of the screen. Clamping to the half-texel inside keeps every fetch
// on the image it belongs to.
float3 SampleRGB(Texture2D tex, float2 uv)
{
	const float2 halfTexel = float2(pc.rcpWidth, pc.rcpHeight) * 0.5f;
	return tex.SampleLevel(g_linearSampler, clamp(uv, halfTexel, 1.0f - halfTexel), 0.0f).rgb;
}

float SampleLuma(Texture2D tex, float2 uv)
{
	return Luma(SampleRGB(tex, uv));
}

float3 LoadRGB(Texture2D tex, int2 coord)
{
	return tex.Load(int3(coord, 0)).rgb;
}

// The FXAA resolve, factored out so the sharpening below can act on its result no matter
// which of the three exits produced it.
float3 ResolveFxaa(float4 svPos)
{
	const int2   coord   = int2(svPos.xy);
	Texture2D    tex     = g_textures[pc.ldrSlot];
	const float2 rcpSize = float2(pc.rcpWidth, pc.rcpHeight);
	const float2 uv      = svPos.xy * rcpSize;

	const float3 rgbM = LoadRGB(tex, coord);

	// Skip FXAA when toggled off - passthrough avoids graph rebuilds.
	if (pc.fxaaEnabled == 0u)
	{
		return rgbM;
	}

	// --- 1. Contrast reject ------------------------------------------------
	const float lumaM = Luma(rgbM);
	const float lumaN = Luma(LoadRGB(tex, coord + int2( 0, -1)));
	const float lumaS = Luma(LoadRGB(tex, coord + int2( 0,  1)));
	const float lumaW = Luma(LoadRGB(tex, coord + int2(-1,  0)));
	const float lumaE = Luma(LoadRGB(tex, coord + int2( 1,  0)));

	const float lumaMin   = min(lumaM, min(min(lumaN, lumaS), min(lumaW, lumaE)));
	const float lumaMax   = max(lumaM, max(max(lumaN, lumaS), max(lumaW, lumaE)));
	const float lumaRange = lumaMax - lumaMin;

	if (lumaRange < max(kAbsoluteThreshold, lumaMax * kRelativeThreshold))
	{
		return rgbM;
	}

	// --- 2. Edge orientation ----------------------------------------------
	const float lumaNW = Luma(LoadRGB(tex, coord + int2(-1, -1)));
	const float lumaNE = Luma(LoadRGB(tex, coord + int2( 1, -1)));
	const float lumaSW = Luma(LoadRGB(tex, coord + int2(-1,  1)));
	const float lumaSE = Luma(LoadRGB(tex, coord + int2( 1,  1)));

	const float edgeH = abs(-2.0f * lumaW + lumaNW + lumaSW)
	                  + abs(-2.0f * lumaM + lumaN  + lumaS ) * 2.0f
	                  + abs(-2.0f * lumaE + lumaNE + lumaSE);
	const float edgeV = abs(-2.0f * lumaN + lumaNW + lumaNE)
	                  + abs(-2.0f * lumaM + lumaW  + lumaE ) * 2.0f
	                  + abs(-2.0f * lumaS + lumaSW + lumaSE);

	const bool isHorizontal = (edgeH >= edgeV);

	// Neighbours across the edge, and which side it actually falls on.
	const float luma1 = isHorizontal ? lumaS : lumaE;
	const float luma2 = isHorizontal ? lumaN : lumaW;
	const float gradient1 = luma1 - lumaM;
	const float gradient2 = luma2 - lumaM;

	const bool is1Steepest = abs(gradient1) >= abs(gradient2);

	// The search terminates when the luma along the edge departs from the local
	// average by a quarter of the gradient across it. Scaling the threshold by
	// the edge's own contrast is what keeps the search from stopping early on a
	// soft edge or running past the end of a hard one.
	const float gradientScaled = 0.25f * max(abs(gradient1), abs(gradient2));

	float stepLength = isHorizontal ? rcpSize.y : rcpSize.x;
	float lumaLocalAverage;
	if (is1Steepest)
	{
		stepLength = -stepLength;
		lumaLocalAverage = 0.5f * (luma1 + lumaM);
	}
	else
	{
		lumaLocalAverage = 0.5f * (luma2 + lumaM);
	}

	// --- 3. Endpoint search ------------------------------------------------
	// Shift half a step across the edge so each tap straddles both sides of it.
	float2 currentUv = uv;
	if (isHorizontal)
	{
		currentUv.y += stepLength * 0.5f;
	}
	else
	{
		currentUv.x += stepLength * 0.5f;
	}

	const float2 offset = isHorizontal ? float2(rcpSize.x, 0.0f) : float2(0.0f, rcpSize.y);

	float2 uv1 = currentUv - offset;
	float2 uv2 = currentUv + offset;

	float lumaEnd1 = SampleLuma(tex, uv1) - lumaLocalAverage;
	float lumaEnd2 = SampleLuma(tex, uv2) - lumaLocalAverage;

	bool reached1 = abs(lumaEnd1) >= gradientScaled;
	bool reached2 = abs(lumaEnd2) >= gradientScaled;

	if (!reached1) { uv1 -= offset; }
	if (!reached2) { uv2 += offset; }

	[unroll]
	for (uint i = 0u; i < kSearchSteps; ++i)
	{
		if (reached1 && reached2)
		{
			break;
		}
		if (!reached1)
		{
			lumaEnd1 = SampleLuma(tex, uv1) - lumaLocalAverage;
			reached1 = abs(lumaEnd1) >= gradientScaled;
		}
		if (!reached2)
		{
			lumaEnd2 = SampleLuma(tex, uv2) - lumaLocalAverage;
			reached2 = abs(lumaEnd2) >= gradientScaled;
		}
		if (!reached1) { uv1 -= offset * kSearchQuality[i]; }
		if (!reached2) { uv2 += offset * kSearchQuality[i]; }
	}

	const float distance1 = isHorizontal ? (uv.x - uv1.x) : (uv.y - uv1.y);
	const float distance2 = isHorizontal ? (uv2.x - uv.x) : (uv2.y - uv.y);

	const bool  isDirection1 = distance1 < distance2;
	const float distanceMin  = min(distance1, distance2);
	const float edgeLength   = distance1 + distance2;

	// Nearer the endpoint means nearer full coverage of the neighbouring pixel.
	const float pixelOffset = -distanceMin / max(edgeLength, 1e-6f) + 0.5f;

	// --- 4. Correct-variation test ----------------------------------------
	// If the endpoint we are closest to varies the same way the centre does,
	// this pixel is not on the transition it looked like it was on, and
	// blending it would smear a feature rather than antialias an edge.
	const bool isLumaCenterSmaller = lumaM < lumaLocalAverage;
	const bool correctVariation =
	    ((isDirection1 ? lumaEnd1 : lumaEnd2) < 0.0f) != isLumaCenterSmaller;

	float finalOffset = correctVariation ? pixelOffset : 0.0f;

	// --- 5. Sub-pixel term -------------------------------------------------
	// Weighted 3x3 average; features smaller than the search can resolve show
	// up here as a gap between the centre and its neighbourhood.
	const float lumaAverage = (1.0f / 12.0f) *
	    (2.0f * (lumaN + lumaS + lumaW + lumaE) + lumaNW + lumaNE + lumaSW + lumaSE);

	const float subPixelOffset1 = saturate(abs(lumaAverage - lumaM) / max(lumaRange, 1e-6f));
	const float subPixelOffset2 = (-2.0f * subPixelOffset1 + 3.0f) * subPixelOffset1 * subPixelOffset1;
	const float subPixelFinal   = subPixelOffset2 * subPixelOffset2 * kSubpixelQuality;

	finalOffset = max(finalOffset, subPixelFinal);

	// --- Final fetch -------------------------------------------------------
	float2 finalUv = uv;
	if (isHorizontal)
	{
		finalUv.y += finalOffset * stepLength;
	}
	else
	{
		finalUv.x += finalOffset * stepLength;
	}

	return SampleRGB(tex, finalUv);
}

// Contrast Adaptive Sharpening (AMD FidelityFX CAS), sharpen-only path.
//
// FXAA is a blur: it reconstructs an edge by fetching between two texels, and everything it
// touches comes back softer. CAS is the natural partner because it is contrast-ADAPTIVE - the
// amount is scaled by how much headroom the local neighbourhood has left, so flat regions are
// untouched and an edge already near black or white is not pushed through it. A fixed unsharp
// mask would halo exactly where FXAA just did its work.
//
// Run on the pre-FXAA neighbourhood and transferred as a delta, because the resolved value is
// only known for THIS pixel - sharpening against resolved neighbours would mean running FXAA
// four more times for one pass that costs less than a tenth of a millisecond.
float3 SharpenDelta(Texture2D tex, int2 coord, float3 center)
{
	if (pc.sharpness <= 0.0f)
	{
		return float3(0.0f, 0.0f, 0.0f);
	}

	// Load clamps out-of-range reads to zero, which on a border pixel is a black tap and a
	// dark rim around the frame. The image size comes from the reciprocals already pushed.
	const int2 maxCoord = int2(int(round(1.0f / pc.rcpWidth)) - 1, int(round(1.0f / pc.rcpHeight)) - 1);
	const float3 n = LoadRGB(tex, clamp(coord + int2( 0, -1), int2(0, 0), maxCoord));
	const float3 s = LoadRGB(tex, clamp(coord + int2( 0,  1), int2(0, 0), maxCoord));
	const float3 w = LoadRGB(tex, clamp(coord + int2(-1,  0), int2(0, 0), maxCoord));
	const float3 e = LoadRGB(tex, clamp(coord + int2( 1,  0), int2(0, 0), maxCoord));

	const float3 mn = min(center, min(min(n, s), min(w, e)));
	const float3 mx = max(center, max(max(n, s), max(w, e)));

	// How much room the neighbourhood has before it clips, per channel. Near 0 where the
	// signal is already at a limit, which is what stops the halo.
	const float3 amp = sqrt(saturate(min(mn, 2.0f - mx) / max(mx, 1e-5f)));

	// -1/8 at the lowest setting, -1/5 at the highest: the neighbour weight of the unsharp
	// kernel, normalised by (1 + 4w) so overall brightness is unchanged.
	const float peak = -1.0f / lerp(8.0f, 5.0f, saturate(pc.sharpness));
	const float3 weight = amp * peak;
	const float3 sharpened = saturate((n * weight + s * weight + w * weight + e * weight + center) / (1.0f + 4.0f * weight));
	return sharpened - center;
}

[shader("fragment")]
float4 fragmentMain(float4 svPos : SV_Position) : SV_Target0
{
	const int2 coord = int2(svPos.xy);
	Texture2D  tex   = g_textures[pc.ldrSlot];

	const float3 resolved = ResolveFxaa(svPos);
	return float4(saturate(resolved + SharpenDelta(tex, coord, LoadRGB(tex, coord))), 1.0f);
}
