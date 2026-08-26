// Procedural gradient sky, rendered as a full-screen pass
// that runs before the forward pass so all opaque geometry draws on top.
//
// Designed to grow: future passes can layer volumetric clouds, atmospheric
// scattering, stars, and a moon by extending EvalSky() or adding passes.
//
// Push constant  (8 bytes):
//   offset 0 : DevicePtr<FrameConstantsData> frame
//
// No descriptor sets - all data comes from the push-constant BDA pointer.

#include "include/DevicePointer.slangh"
#include "include/FrameConstants.slangh"
#include "include/Sky.slangh"

struct SkyboxPush
{
    DevicePtr<FrameConstantsData> frame;
};
[[vk::push_constant]] SkyboxPush pc;

// ── Vertex I/O ────────────────────────────────────────────────────────────

struct VSOut
{
    float4 svPos : SV_Position;
    float2 ndc   : TEXCOORD0;   // clip-space XY (== NDC XY since w = 1)
};

// ── Vertex shader ──────────────────────────────────────────────────────────
// Produces a full-screen CCW triangle from SV_VertexID - no vertex buffer.
//   ID 0 -> NDC (-1, -1)   top-left  (Vulkan: top-left since Y is down)
//   ID 1 -> NDC ( 3, -1)   right overshoot
//   ID 2 -> NDC (-1,  3)   bottom overshoot
// Rasteriser clips the overshoots to the viewport.

[shader("vertex")]
VSOut vertexMain(uint vertexID : SV_VertexID)
{
    const float2 uv    = float2((vertexID << 1u) & 2u, vertexID & 2u);
    const float2 ndcXY = uv * 2.0f - 1.0f;
    VSOut o;
    o.svPos = float4(ndcXY, 0.0f, 1.0f);
    o.ndc   = ndcXY;
    return o;
}

// ── Sky evaluation ────────────────────────────────────────────────────────
// Returns a linear-HDR sky colour for a given world-space view direction.
//
// horizonColor  - base colour near the horizon (typically a brightened
//                 sky-horizon tint supplied by the app.
// sunDir        - normalised world-space direction TOWARDS the sun.
// sunIntensity  - directional light intensity scalar.

[shader("fragment")]
float4 fragmentMain(VSOut input) : SV_Target0
{
    FrameConstantsData* fc = pc.frame.Ptr();

    // WYSIWYG background modes (solid colour / gradient) are composited in the
    // tonemap pass in display space. The sky pass only marks these pixels as
    // "not scene" by writing zero coverage (alpha 0); the compositor then fills
    // them from the camera's background params. skyVoidColor.w < 0.5 signals a
    // non-sky background (set on the game thread from the camera's mode).
    if (fc->skyVoidColor.w < 0.5f)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    // ── Step 1: NDC -> view-space direction ──────────────────────────────
    // GLM perspective (Vulkan variant) column-major layout:
    //   proj[0][0]  = 1 / (aspect * tan(fovY/2))   - col 0, row 0
    //   proj[1][1]  = -1 / tan(fovY/2)              - col 1, row 1  (Y-flip)
    //   proj[2][3]  = -1                             - col 2, row 3  (w = -viewZ)
    //
    // Inverting the perspective divide:
    //   ndc.x = proj[0][0] * viewX  ->  viewX = ndc.x / proj[0][0]
    //   ndc.y = proj[1][1] * viewY  ->  viewY = ndc.y / proj[1][1]
    //   viewZ = -1  (ray shoots into the scene)
    const float viewX = input.ndc.x / fc->proj[0][0];
    const float viewY = input.ndc.y / fc->proj[1][1];
    const float3 viewDir = normalize(float3(viewX, viewY, -1.0f));

    // ── Step 2: view-space -> world-space ────────────────────────────────
    // fc->view is the world->view rotation R (column-major, same convention
    // as model matrix). The inverse of an orthogonal rotation is its
    // transpose, which is equivalent to multiplying the view-space row
    // vector by R directly.
    const float3x3 viewRot   = float3x3(fc->view[0].xyz, fc->view[1].xyz, fc->view[2].xyz);
    const float3   worldDir  = normalize(mul(viewDir, viewRot));

    // ── Step 3: evaluate sky ─────────────────────────────────────────────
    const float3 sunDir       = normalize(fc->sunDirectionIntensity.xyz);
    const float  sunIntensity = fc->sunDirectionIntensity.w;
    const float3 sunColor     = fc->sunColor.rgb;
    const float3 horizonColor = fc->skyHorizonColor.rgb;
    const float3 zenithColor  = fc->skyZenithColor.rgb;
    const float3 voidColor    = fc->skyVoidColor.rgb;

    float3 sky = EvalSky(
        worldDir,
        sunDir,
        sunColor,
        sunIntensity,
        horizonColor,
        zenithColor,
        voidColor);

    // Tiny dither helps hide residual daytime gradient quantization.
    const float dayAmount = saturate(sunDir.y * 0.5f + 0.5f);
    const float dither = Hash12(floor(input.svPos.xy)) - 0.5f;
    sky += dither * (1.0f / 1024.0f) * dayAmount;

    return float4(max(sky, float3(0.0f, 0.0f, 0.0f)), 1.0f);
}
