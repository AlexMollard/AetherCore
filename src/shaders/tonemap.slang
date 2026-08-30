// Post-process pass: HDR tonemap + gamma correction.
//
// Vertex shader generates a full-screen triangle from SV_VertexID so no
// vertex buffer is required.  The fragment shader reads the HDR offscreen
// buffer directly via a Load() call (integer pixel coordinates) to avoid
// needing a sampler, then applies the selected tonemap operator.
//
// Binding layout for this standalone pass (no per-draw set 0):
//   set 0, binding 0 - bindless sampled images
// This differs from the forward-pass contract (which uses set 1) because
// there is no per-draw descriptor set in a full-screen post-process pass.

// Bindless sampled images at set 0 (no per-draw set in this pass).
[[vk::binding(0, 0)]]
Texture2D g_textures[];

[[vk::binding(1, 0)]]
SamplerState g_linearSampler;

#include "include/FrameConstants.slangh"
#include "include/FullScreenTriangle.slangh"
#include "include/ShadowSampling.slangh"

// Push constants.
struct TonemapPush
{
    uint  hdrSlot;
    uint  mode;           // tonemap operator index
    float exposure;
    uint  debugCompare;   // 0 = normal, 1 = side-by-side comparison
    uint  debugModeCount; // number of modes to compare (0 = all)
    int   inspectX;       // pixel inspect X (-1 = disabled)
    int   inspectY;
    uint  screenWidth;    // render target width for debug division
    uint  screenHeight;   // render target height for header bar
    uint  bloomSlot;      // bloom chain mip 0 (0xFFFFFFFF = bloom off)
    float bloomStrength;  // linear blend toward the bloom result
    uint  dofSlot;        // half-res out-of-focus image (0xFFFFFFFF = lens off)
    uint64_t backgroundParamsAddr; // BDA -> GpuBackgroundParams (0 = disabled)
    float gradeContrast;    // 1 = unchanged
    float gradeSaturation;  // 1 = unchanged, 0 = greyscale
    float gradeTemperature; // 0 = unchanged, >0 warmer
    float gradeTint;        // 0 = unchanged, >0 greener
    float vignetteIntensity; // 0 = off
    float vignetteRoundness; // 1 = circular, 0 = follows the aspect ratio
    uint  motionBlurDepthSlot;      // scene depth (0xFFFFFFFF = no depth, blur off)
    float motionBlurStrength;       // shutter fraction of the frame interval; 0 = off
    float motionBlurMaxRadiusPixels;// ceiling, so a teleport cannot smear the screen
    uint  _padMotionBlur;
    uint64_t frameConstantsAddr;    // BDA -> FrameConstantsData (0 = motion blur off)
    float chromaticAberration;      // pixels of channel separation at the corner; 0 = off
};
[[vk::push_constant]] TonemapPush pc;

// Colour grade, in LINEAR space and BEFORE the tonemap curve.
//
// Order matters and this is the order a grading desk uses: white balance, then saturation,
// then contrast. Doing it before the curve is the point - grading display-referred values
// fights the operator, because the curve has already compressed the highlights that a
// contrast push most wants to move.
//
// Every term is written so its neutral value is an exact identity, not an approximate one:
// at contrast 1, saturation 1 and no white balance the input is returned unchanged, so a
// project that never touches these renders bit-for-bit as it did before.
// True when the grade would return its input unchanged. Used to skip the background's
// decode/grade/re-encode round trip entirely, so an ungraded project keeps its camera colour
// EXACTLY as authored rather than approximately - pow(pow(x, 2.2), 1/2.2) is only x to within
// float error, and "the colour I picked" is the whole promise of that setting.
bool GradeIsIdentity()
{
    return pc.gradeContrast == 1.0f && pc.gradeSaturation == 1.0f
        && pc.gradeTemperature == 0.0f && pc.gradeTint == 0.0f;
}

float3 ApplyGrade(float3 color)
{
    // Luminance weights, used by both the white balance and the saturation term.
    const float3 kLuma = float3(0.2126f, 0.7152f, 0.0722f);

    // White balance. Temperature trades red against blue, tint trades green against
    // magenta - the two axes a camera actually exposes. The result is renormalised by its
    // own luminance so that warming an image does not also brighten it; without that the
    // control doubles as an exposure slider and stops being usable.
    if (pc.gradeTemperature != 0.0f || pc.gradeTint != 0.0f)
    {
        float3 balance = float3(1.0f + pc.gradeTemperature,
                                1.0f + pc.gradeTint,
                                1.0f - pc.gradeTemperature);
        balance = max(balance, float3(0.0f, 0.0f, 0.0f));
        const float norm = max(dot(balance, kLuma), 1e-4f);
        color *= balance / norm;
    }

    // Saturation about the pixel's own luminance. Values above 1 extrapolate, which can
    // push a channel negative on already-saturated colours, so clamp at zero.
    if (pc.gradeSaturation != 1.0f)
    {
        const float luma = dot(color, kLuma);
        color = max(lerp(float3(luma, luma, luma), color, pc.gradeSaturation), 0.0f);
    }

    // Contrast about middle grey. The pivot has to be a scene-referred mid-tone rather
    // than 0.5: in linear space 0.5 is nearly two stops above middle grey, so pivoting
    // there darkens the entire image the moment contrast is raised.
    if (pc.gradeContrast != 1.0f)
    {
        const float kPivot = 0.18f;
        color = kPivot * pow(max(color, 1e-6f) / kPivot, pc.gradeContrast);
    }

    return color;
}

// Lens vignette: the falloff a real lens has toward the edge of its image circle.
//
// Returns the falloff as a scalar so both the scene and the camera background can use the
// same curve. The scene is dimmed in LINEAR light, before the tonemap: a lens dims the light
// arriving at the sensor, so the curve should see the already-dimmed value and a bright
// corner highlight then rolls down the shoulder the way it would in camera. Scaling the
// display-referred pixel instead just greys the corner and reads as a dirty screen.
//
// The background is the exception and is multiplied after the curve - see the note at its
// composite, which explains why it has to be dimmed at all.
//
// Roundness selects what "the edge" means: at 1 the falloff is circular in screen space, at
// 0 it follows the aspect ratio, so a wide frame darkens at the sides rather than in a
// circle that never reaches the corners.
float VignetteFactor(float2 uv)
{
    if (pc.vignetteIntensity <= 0.0f)
    {
        return 1.0f;
    }

    float2 centered = uv * 2.0f - 1.0f;
    const float aspect = float(max(pc.screenWidth, 1u)) / float(max(pc.screenHeight, 1u));
    // Stretch x back toward square by however round the falloff is asked to be.
    centered.x *= lerp(1.0f, aspect, saturate(pc.vignetteRoundness));

    // cos^4 is the classic natural-vignetting falloff; using the squared radius keeps it a
    // smooth curve to the corner rather than a ring with a visible edge.
    const float r2 = saturate(dot(centered, centered) * 0.5f);
    const float falloff = 1.0f - r2 * r2;

    return lerp(1.0f, falloff, saturate(pc.vignetteIntensity));
}

// Camera-owned WYSIWYG background, composited in display space (see
// PostProcessStack::SetBackgroundParams). Layout matches the CPU fill.
struct GpuBackgroundParams
{
    uint   mode;         // 0 solid, 1 gradient
    uint   stopCount;
    float  angleRadians; // 0 = top->bottom
    uint   _pad;
    float4 stops[8];     // xyz = display-space colour, w = position (0..1)
};

// Evaluate the background colour in display space for a 0..1 screen UV (y-down).
float3 EvalBackground(GpuBackgroundParams* bg, float2 uv)
{
    if (bg->mode == 0u || bg->stopCount <= 1u)
    {
        return bg->stops[0].rgb;
    }

    // Project the pixel onto the gradient axis (0 deg = top->bottom).
    const float2 dir = float2(sin(bg->angleRadians), cos(bg->angleRadians));
    const float  t   = saturate(dot(uv - 0.5f, dir) + 0.5f);

    float3 col = bg->stops[0].rgb;
    for (uint i = 1u; i < bg->stopCount; ++i)
    {
        const float p0 = bg->stops[i - 1u].w;
        const float p1 = bg->stops[i].w;
        const float seg = saturate((t - p0) / max(p1 - p0, 1e-5f));
        col = (t >= p0) ? lerp(bg->stops[i - 1u].rgb, bg->stops[i].rgb, seg) : col;
    }
    return col;
}

// Tonemap operators live in individual family files under include/Tonemap/.
// Add a new operator there, then register it in TonemapOperators.slangh.
#include "include/Tonemap/TonemapOperators.slangh"

// ── Debug header bar: operator labels ──────────────────────────────────────
// Each vertical strip gets a colored header bar with the operator index digit.
// Must match kTonemapCount in TonemapDefs.hpp.
static const uint kNumOperators = 11u;

// 3×5 bitmap font digits (0 = empty, 1 = filled), rows packed as 3-bit values.
static const uint kDigitRows[10][5] = {
    {7u, 5u, 5u, 5u, 7u}, // 0: ###, #.#, #.#, #.#, ###
    {1u, 1u, 1u, 1u, 1u}, // 1: ..#, ..#, ..#, ..#, ..#
    {7u, 1u, 7u, 4u, 7u}, // 2: ###, ..#, ###, #.., ###
    {7u, 1u, 7u, 1u, 7u}, // 3: ###, ..#, ###, ..#, ###
    {5u, 5u, 7u, 1u, 1u}, // 4: #.#, #.#, ###, ..#, ..#
    {7u, 4u, 7u, 1u, 7u}, // 5: ###, #.., ###, ..#, ###
    {7u, 4u, 7u, 5u, 7u}, // 6: ###, #.., ###, #.#, ###
    {7u, 1u, 1u, 1u, 1u}, // 7: ###, ..#, ..#, ..#, ..#
    {7u, 5u, 7u, 5u, 7u}, // 8: ###, #.#, ###, #.#, ###
    {7u, 5u, 7u, 1u, 1u}, // 9: ###, #.#, ###, ..#, ..#
};

// Unique hue per operator for the header bar.
static const float3 kBarColors[11] = {
    float3(0.78f, 0.39f, 0.39f), // 0 Reinhard      - muted red
    float3(0.39f, 0.78f, 0.39f), // 1 ACES Filmic   - muted green
    float3(0.39f, 0.39f, 0.78f), // 2 Uncharted 2   - muted blue
    float3(0.78f, 0.78f, 0.31f), // 3 HejlRichard    - yellow
    float3(0.47f, 0.47f, 0.47f), // 4 Linear         - gray
    float3(0.78f, 0.47f, 0.78f), // 5 Exponential    - pink
    float3(0.31f, 0.78f, 0.78f), // 6 Filmic Dice    - cyan
    float3(0.78f, 0.59f, 0.31f), // 7 Lottes         - orange
    float3(0.31f, 0.78f, 0.59f), // 8 RomBinDaHouse  - teal
    float3(1.00f, 1.00f, 1.00f), // 9 Vanilla        - white
    float3(0.59f, 0.47f, 0.78f), // 10 AgX           - violet
};

// ── Fragment shader ────────────────────────────────────────────────────────
// Triangular-PDF dither, applied in display space immediately before the 8-bit
// write.
//
// A smooth gradient across a large area - a sky, a soft falloff, a fog layer -
// has far more distinct values than 8 bits can hold, so quantisation collapses
// runs of rows onto one level and the eye reads the steps between them as
// contour lines. Mach banding makes them look like hard edges even though the
// step is a single LSB. ACES makes it worse, because its toe compresses the
// darks where the steps are already widest.
//
// Adding sub-LSB noise before rounding turns the hard decision into a
// probabilistic one: a pixel lands on the higher level in proportion to how far
// between the two it really was, so the average over any small area carries the
// original value and the contour disappears.
//
// The distribution matters. Uniform noise removes the banding but leaves the
// residual error correlated with the signal, which shows up as noise that
// visibly breathes across the gradient. Subtracting two uniform samples gives a
// triangular PDF, which decorrelates it - the standard result from audio
// dithering, and the reason TPDF is what everyone ends up at.
float Hash12Dither(float2 p)
{
	return frac(sin(dot(p, float2(12.9898f, 78.233f))) * 43758.5453f);
}

// One LSB peak-to-peak, independent per channel so the noise stays neutral
// rather than tinting toward whichever axis the hash happens to correlate on.
float3 DitherTriangular(float2 pixel)
{
	const float r = Hash12Dither(pixel) - Hash12Dither(pixel + float2(37.0f, 17.0f));
	const float g = Hash12Dither(pixel + float2(11.0f, 91.0f)) - Hash12Dither(pixel + float2(59.0f, 3.0f));
	const float b = Hash12Dither(pixel + float2(73.0f, 41.0f)) - Hash12Dither(pixel + float2(23.0f, 67.0f));
	return float3(r, g, b) * (1.0f / 255.0f);
}

// Camera motion blur: a shutter that stays open for part of the frame.
//
// A real camera integrates light over its exposure, so anything whose image moves during
// it lands smeared along the path it travelled. A rasteriser samples one instant, which is
// why a rendered pan looks unnaturally crisp and, at low frame rates, judders.
//
// Reconstructed here without a velocity buffer, because CAMERA motion does not need one: a
// static world point's screen position follows entirely from its depth and the
// view-projection, so reprojecting this pixel through LAST frame's matrix says exactly
// where it used to be. The difference is its screen velocity and averaging along that
// segment is the missing integral.
//
// Done here rather than in a pass of its own for the same reason the grade is: this is
// linear HDR, before the curve, where a bright streak still carries its real energy. It
// also costs no extra render target and no extra full-screen pass.
//
// The limitation, stated rather than discovered later: this is camera motion ONLY. An
// object crossing a stationary camera is not blurred, and one moving with the camera is
// blurred as though it were scenery. Per-object motion needs previous transforms in a
// velocity buffer; this is the half that costs one matrix.
static const uint kMotionBlurTaps = 12u;

// Below this the streak is narrower than the pixel it is drawn on, so there is nothing to
// integrate and every tap lands on the same texel.
static const float kMotionBlurMinPixels = 0.75f;

// How much farther than the centre a tap may be before it counts as background rather than
// as part of this surface. Relative, because a metre of separation is a different surface
// up close and the same one far away.
static const float kMotionBlurDepthRejectRatio = 1.05f;

float MotionBlurViewDistance(float ndcDepth, float2 uv, FrameConstantsData* fc)
{
    const float4 clip = float4(uv * 2.0f - 1.0f, ndcDepth, 1.0f);
    const float4 world = mul(fc->invViewProj, clip);
    return distance(world.xyz / max(world.w, 1e-6f), fc->cameraWorldPos.xyz);
}

float3 ApplyCameraMotionBlur(float3 centre, int2 coord, float2 svPos)
{
    if (pc.motionBlurStrength <= 0.0f || pc.frameConstantsAddr == 0 || pc.motionBlurDepthSlot == 0xFFFFFFFFu)
    {
        return centre;
    }

    FrameConstantsData* fc = (FrameConstantsData*)pc.frameConstantsAddr;

    const float2 size = float2(pc.screenWidth, pc.screenHeight);
    if (size.x < 1.0f || size.y < 1.0f)
    {
        return centre;
    }
    const float2 uv = (float2(coord) + 0.5f) / size;

    const float depth = g_textures[pc.motionBlurDepthSlot].Load(int3(coord, 0)).r;
    // Sky sits at infinity, where reprojection is degenerate: a rotating camera would smear
    // it across the whole screen, which is both wrong and the most expensive case there is.
    if (depth >= 1.0f)
    {
        return centre;
    }

    const float4 clip = float4(uv * 2.0f - 1.0f, depth, 1.0f);
    const float4 world = mul(fc->invViewProj, clip);
    const float3 worldPos = world.xyz / max(world.w, 1e-6f);

    const float4 prevClip = mul(fc->prevViewProj, float4(worldPos, 1.0f));
    if (abs(prevClip.w) < 1e-6f)
    {
        return centre;
    }
    const float2 prevUv = (prevClip.xy / prevClip.w) * 0.5f + 0.5f;
    const float centreDistance = distance(worldPos, fc->cameraWorldPos.xyz);

    // The shutter was open for only part of the interval, so the image travelled that far.
    float2 velocityUv = (uv - prevUv) * pc.motionBlurStrength;
    const float lengthPixels = length(velocityUv * size);
    if (lengthPixels < kMotionBlurMinPixels)
    {
        return centre;
    }
    if (lengthPixels > pc.motionBlurMaxRadiusPixels)
    {
        velocityUv *= pc.motionBlurMaxRadiusPixels / lengthPixels;
    }

    // Jittered so the taps do not land on identical positions in every pixel, which shows
    // the streak as a row of discrete copies. Interleaved gradient noise is stable frame to
    // frame, so it reads as grain rather than as crawl.
    const float jitter = InterleavedGradientNoise(svPos);

    float3 sum = centre;
    float weight = 1.0f;

    [unroll]
    for (uint i = 0u; i < kMotionBlurTaps; ++i)
    {
        // Centred on the pixel and spread over the whole segment, so the streak sits
        // symmetrically about where the surface is rather than lagging behind it.
        const float t = ((float(i) + jitter) / float(kMotionBlurTaps)) - 0.5f;
        const float2 tapUv = uv + velocityUv * t;
        if (any(tapUv < 0.0f) || any(tapUv > 1.0f))
        {
            continue;
        }

        const int2 tapCoord = int2(tapUv * size);
        const float tapDepth = g_textures[pc.motionBlurDepthSlot].Load(int3(tapCoord, 0)).r;
        // Reject anything BEHIND this surface: without it a fast pan drags the background
        // across a near silhouette and the object grows a halo of whatever was behind it,
        // the classic tell of naive motion blur. Nearer taps are kept deliberately - a
        // foreground object genuinely should smear over its background.
        if (tapDepth >= 1.0f || MotionBlurViewDistance(tapDepth, tapUv, fc) > centreDistance * kMotionBlurDepthRejectRatio)
        {
            continue;
        }

        sum += g_textures[pc.hdrSlot].Load(int3(tapCoord, 0)).rgb;
        weight += 1.0f;
    }

    return sum / max(weight, 1.0f);
}

// Lateral chromatic aberration: a lens brings short wavelengths to focus slightly closer to
// the axis than long ones, so the three channels land at different radii. The separation is
// zero on the optical axis and grows toward the edge - hence the radius-squared falloff
// rather than a constant offset, which would smear the centre of frame where a real lens is
// at its sharpest.
//
// Sampled from the HDR target before anything else runs, because on a real camera this
// happens at the glass, ahead of every effect the renderer applies afterwards.
float3 SampleSceneWithAberration(int2 coord)
{
    if (pc.chromaticAberration <= 0.0f)
    {
        // The original path exactly: an unfiltered load, so a project that never turns this
        // on renders bit-for-bit as it did before.
        return g_textures[pc.hdrSlot].Load(int3(coord, 0)).rgb;
    }

    const float2 dims = float2(pc.screenWidth, pc.screenHeight);
    const float2 uv = (float2(coord) + 0.5f) / dims;
    const float2 centered = uv * 2.0f - 1.0f;

    // Authored in PIXELS rather than UV, so the look survives a resolution change instead of
    // doubling when the frame does. 0.5 normalises the radial term to 1 at the corner.
    const float2 offset = centered * dot(centered, centered) * 0.5f * pc.chromaticAberration / dims;

    // Saturate rather than trust the sampler's addressing: an offset near the border would
    // otherwise be free to wrap and pull the opposite edge into frame.
    const float3 outward = g_textures[pc.hdrSlot].SampleLevel(g_linearSampler, saturate(uv + offset), 0.0).rgb;
    const float3 middle  = g_textures[pc.hdrSlot].SampleLevel(g_linearSampler, uv, 0.0).rgb;
    const float3 inward  = g_textures[pc.hdrSlot].SampleLevel(g_linearSampler, saturate(uv - offset), 0.0).rgb;

    return float3(outward.r, middle.g, inward.b);
}

[shader("fragment")]
float4 fragmentMain(float4 svPos : SV_Position) : SV_Target0
{
    const int2 coord = int2(svPos.xy);
    float3 scene = SampleSceneWithAberration(coord);

    // Before bloom and the grade: a streak should carry its real energy into both.
    scene = ApplyCameraMotionBlur(scene, coord, svPos.xy);

    // Bloom is mixed in linear HDR, before exposure and the operator, so it is
    // tonemapped along with everything else. Added afterwards it would sit on top of
    // the curve and read as a flat wash rather than light.
    //
    // Lerp rather than add: the chain already carries the scene's own energy, so
    // adding it would brighten the whole image as the strength rises. Blending
    // redistributes that energy into the glow and leaves overall exposure alone.
    // Defocus is mixed in before the bloom blend, but that is NOT the same as bloom
    // seeing it: the bloom chain downsamples the HDR target directly, which is still
    // sharp at that point. So an out-of-focus highlight glows from the point it would
    // have been rather than from the disc it actually covers.
    //
    // Left that way deliberately. Bloom is a wide blur already, so the difference is
    // small, and closing it would mean writing the defocused image back to a second
    // full-resolution target for the chain to read - a real cost for a subtle gain on a
    // pass that only runs when a camera has a lens at all.
    if (pc.dofSlot != 0xFFFFFFFFu)
    {
        const float2 dofUv = (float2(coord) + 0.5f) / float2(pc.screenWidth, pc.screenHeight);
        const float4 blurred = g_textures[pc.dofSlot].SampleLevel(g_linearSampler, dofUv, 0.0);
        scene = lerp(scene, blurred.rgb, blurred.a);
    }

    if (pc.bloomSlot != 0xFFFFFFFFu)
    {
        const float2 uv = (float2(coord) + 0.5) / float2(pc.screenWidth, pc.screenHeight);
        const float3 bloom = g_textures[pc.bloomSlot].SampleLevel(g_linearSampler, uv, 0.0).rgb;
        scene = lerp(scene, bloom, saturate(pc.bloomStrength));
    }

    // Graded after exposure and before the operator: exposure decides what part of the
    // scene's range the curve sees, and the grade shapes what lands there.
    const float2 gradeUv = (float2(coord) + 0.5f) / float2(pc.screenWidth, pc.screenHeight);
    const float vignette = VignetteFactor(gradeUv);
    const float3 hdr = ApplyGrade(scene * pc.exposure) * vignette;

    float3 mapped;

    if (pc.debugCompare != 0u)
    {
        // Side-by-side: divide into vertical strips, one operator per strip.
        uint numModes = (pc.debugModeCount != 0u) ? pc.debugModeCount : kNumOperators;
        uint stripIdx = (uint(coord.x) * numModes) / max(pc.screenWidth, 1u);

        // Compute strip pixel bounds so the header bar can position its digit.
        uint stripStart = (stripIdx * pc.screenWidth) / numModes;
        uint stripEnd   = ((stripIdx + 1u) * pc.screenWidth) / numModes;
        uint stripWidth = stripEnd - stripStart;

        // ── Operator-identifying header bar ────────────────────────────
        const uint kBarHeight = 24u;
        if (uint(coord.y) < kBarHeight)
        {
            float3 barColor = kBarColors[stripIdx];

            // Render the operator index digit (0–9) using a 3×5 bitmap font
            // with 4×4 screen-pixel blocks per font pixel.
            // kDigitRows is a font indexed by digit, not by operator, so an
            // operator index of 10 or more has to be spelled out across two
            // glyphs rather than used as a row directly.
            uint localX    = uint(coord.x) - stripStart;
            uint glyphW    = 12u; // 3 font cols × 4 px
            uint glyphGap  = 4u;
            uint digitH    = 20u; // 5 font rows × 4 px
            uint numDigits = (stripIdx >= 10u) ? 2u : 1u;
            uint labelW    = numDigits * glyphW + (numDigits - 1u) * glyphGap;
            uint digitOff  = (stripWidth > labelW) ? (stripWidth - labelW) / 2u : 0u;

            if (localX >= digitOff && localX < digitOff + labelW &&
                uint(coord.y) < digitH)
            {
                uint labelX = localX - digitOff;
                uint glyph  = labelX / (glyphW + glyphGap);
                uint inX    = labelX - glyph * (glyphW + glyphGap);
                if (inX < glyphW)
                {
                    uint digit = (numDigits == 2u)
                        ? ((glyph == 0u) ? (stripIdx / 10u) : (stripIdx % 10u))
                        : stripIdx;
                    uint fontX = inX / 4u;
                    uint fontY = uint(coord.y) / 4u;
                    if (((kDigitRows[min(digit, 9u)][fontY] >> (2u - fontX)) & 1u) != 0u)
                    {
                        barColor = float3(1.0f, 1.0f, 1.0f);
                    }
                }
            }

            mapped = barColor;
        }
        else
        {
            mapped = TonemapByIndex(stripIdx, hdr);
        }
    }
    else
    {
        mapped = TonemapByIndex(pc.mode, hdr);
    }

    // Approximate sRGB gamma (linear -> display).
    float3 outColor = pow(max(mapped, float3(0.0f)), float3(1.0f / 2.2f));

    // WYSIWYG background composite: fill pixels the scene did not cover (HDR
    // alpha) with the camera background, evaluated directly in display space so
    // it matches the authored swatch. Disabled (addr 0) for the sky mode.
    if (pc.backgroundParamsAddr != 0ull)
    {
        GpuBackgroundParams* bg = (GpuBackgroundParams*)pc.backgroundParamsAddr;
        const float  coverage = g_textures[pc.hdrSlot].Load(int3(coord, 0)).a;
        const float2 uv = (float2(coord) + 0.5f) / float2(max(pc.screenWidth, 1u), max(pc.screenHeight, 1u));
        // The vignette dims the background too. It is a LENS effect, so it darkens
        // everything the lens sees - leaving the backdrop at full brightness puts a bright
        // border around a darkened scene, which reads as a bug rather than as a lens. It
        // matters most exactly where the background is most of the frame: a 2D scene on a
        // flat colour, where otherwise the vignette would have almost nothing to act on.
        //
        // Applied here as a straight multiply rather than in linear light like the scene,
        // because this colour is display-referred by design - the author picked the pixel
        // they want to see - and never passes through the tonemap curve at all, so there is
        // no shoulder for it to roll down.
        float3 bgCol = EvalBackground(bg, uv);

        // The grade covers the background as well. A look applied to everything except the
        // sky is not a look: desaturate a 2D game and the backdrop would stay the one
        // coloured thing on screen, which is worse than not grading at all.
        //
        // Decoded to linear first, graded, then re-encoded, rather than graded where it sits.
        // ApplyGrade pivots contrast at 0.18 because that is middle grey in LINEAR light; in
        // display-referred values middle grey is nearer 0.46, so grading this colour in place
        // would pivot around the wrong tone and darken every background the moment contrast
        // moved. Going through linear also means there is exactly ONE definition of the look,
        // rather than a second copy with its own pivot to keep in step.
        //
        // 2.2 is the same approximation the scene is encoded with a few lines above, so the
        // round trip is consistent with this shader's own convention rather than a new
        // assumption about the display.
        if (!GradeIsIdentity())
        {
            const float3 bgLinear = pow(max(bgCol, float3(0.0f, 0.0f, 0.0f)), float3(2.2f, 2.2f, 2.2f));
            const float3 graded = max(ApplyGrade(bgLinear), float3(0.0f, 0.0f, 0.0f));
            bgCol = pow(graded, float3(1.0f / 2.2f, 1.0f / 2.2f, 1.0f / 2.2f));
        }

        bgCol *= vignette;
        outColor = lerp(bgCol, outColor, saturate(coverage));
    }

    // Last thing before the 8-bit target: everything above, including the
    // background composite, is a candidate for banding.
    outColor += DitherTriangular(float2(coord));

    return float4(outColor, 1.0f);
}
