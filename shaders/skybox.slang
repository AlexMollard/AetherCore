// skybox.slang — Procedural gradient sky, rendered as a full-screen pass
// that runs before the forward pass so all opaque geometry draws on top.
//
// Designed to grow: future passes can layer volumetric clouds, atmospheric
// scattering, stars, and a moon by extending EvalSky() or adding passes.
//
// Push constant  (8 bytes):
//   offset 0 : uint64_t  frameAddr   BDA → FrameConstantsData
//
// No descriptor sets — all data comes from the push-constant BDA pointer.

// ── FrameConstantsData ────────────────────────────────────────────────────
// Mirrors aether::FrameConstants exactly (see FrameConstants.hpp).
struct FrameConstantsData
{
    [[vk::offset(0)]]   float4x4 viewProj;
    [[vk::offset(64)]]  float4x4 view;
    [[vk::offset(128)]] float4x4 proj;
    // offsets 192–207  materialBufferAddr / _pad0 — not accessed here
    [[vk::offset(208)]] float4 sunDirectionIntensity; // xyz = world-space dir, w = intensity
    [[vk::offset(224)]] float4 ambientColor;          // rgb = base sky colour, a = unused
    [[vk::offset(240)]] float4 cameraWorldPos;        // xyz = camera pos, w = 1
};

struct SkyboxPush
{
    uint64_t frameAddr;
};
[[vk::push_constant]] SkyboxPush pc;

// ── Vertex I/O ────────────────────────────────────────────────────────────

struct VSOut
{
    float4 svPos : SV_Position;
    float2 ndc   : TEXCOORD0;   // clip-space XY (== NDC XY since w = 1)
};

// ── Vertex shader ──────────────────────────────────────────────────────────
// Produces a full-screen CCW triangle from SV_VertexID — no vertex buffer.
//   ID 0 → NDC (-1, -1)   top-left  (Vulkan: top-left since Y is down)
//   ID 1 → NDC ( 3, -1)   right overshoot
//   ID 2 → NDC (-1,  3)   bottom overshoot
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
// horizonColor  — base colour near the horizon (typically a brightened
//                 version of ambientColor.rgb).
// sunDir        — normalised world-space direction TOWARDS the sun.
// sunIntensity  — directional light intensity scalar.

float3 EvalSky(float3 worldDir, float3 sunDir, float sunIntensity, float3 horizonColor)
{
    // Simple two-band gradient: horizon (t = 0) → zenith (t = 1).
    // Squared ramp gives a subtle atmospheric density feel.
    const float t         = saturate(worldDir.y * 0.5f + 0.5f);
    const float3 zenith   = horizonColor * 0.30f;
    float3 sky            = lerp(horizonColor, zenith, t * t);

    // Below the horizon: darken gently to avoid a sharp cutoff.
    const float belowHorizon = saturate(-worldDir.y * 4.0f);
    sky = lerp(sky, sky * 0.15f, belowHorizon);

    // Sun disk: sharp crisp edge.
    const float cosAngle = dot(worldDir, sunDir);
    const float sunDisk  = smoothstep(0.9996f, 1.0f, cosAngle);

    // Wide glow halo around the sun.
    const float sunHalo  = smoothstep(0.94f, 1.0f, cosAngle) * 0.20f;

    // Warm white for the disk, orange-amber for the halo.
    sky += sunDisk * float3(1.5f, 1.35f, 1.1f)  * sunIntensity;
    sky += sunHalo * float3(1.0f, 0.65f, 0.25f) * sunIntensity;

    return sky;
}

// ── Fragment shader ────────────────────────────────────────────────────────

[shader("fragment")]
float4 fragmentMain(VSOut input) : SV_Target0
{
    FrameConstantsData* fc = (FrameConstantsData*)pc.frameAddr;

    // ── Step 1: NDC → view-space direction ──────────────────────────────
    // GLM perspective (Vulkan variant) column-major layout:
    //   proj[0][0]  = 1 / (aspect * tan(fovY/2))   — col 0, row 0
    //   proj[1][1]  = -1 / tan(fovY/2)              — col 1, row 1  (Y-flip)
    //   proj[2][3]  = -1                             — col 2, row 3  (w = -viewZ)
    //
    // Inverting the perspective divide:
    //   ndc.x = proj[0][0] * viewX  →  viewX = ndc.x / proj[0][0]
    //   ndc.y = proj[1][1] * viewY  →  viewY = ndc.y / proj[1][1]
    //   viewZ = -1  (ray shoots into the scene)
    const float viewX = input.ndc.x / fc->proj[0][0];
    const float viewY = input.ndc.y / fc->proj[1][1];
    const float3 viewDir = normalize(float3(viewX, viewY, -1.0f));

    // ── Step 2: view-space → world-space ────────────────────────────────
    // fc->view is the world→view rotation R (column-major, same convention
    // as model matrix).  The inverse of an orthogonal rotation is its
    // transpose: worldDir = R^T * viewDir.
    //
    // In SPIR-V column-major: float3x3(M[0].xyz, M[1].xyz, M[2].xyz)
    // constructs a matrix whose columns are those vectors, giving the
    // upper-left 3×3 of M directly.  Transposing it yields R^T.
    const float3x3 viewRot   = float3x3(fc->view[0].xyz, fc->view[1].xyz, fc->view[2].xyz);
    const float3   worldDir  = normalize(mul(transpose(viewRot), viewDir));

    // ── Step 3: evaluate sky ─────────────────────────────────────────────
    const float3 sunDir      = normalize(fc->sunDirectionIntensity.xyz);
    const float  sunIntensity = fc->sunDirectionIntensity.w;

    // Drive horizon colour from ambientColor.  Scale up so the sky reads
    // brighter than the ambient hemisphere term used in scene lighting.
    const float3 horizonColor = max(fc->ambientColor.rgb * 7.0f, float3(0.04f, 0.06f, 0.10f));

    return float4(EvalSky(worldDir, sunDir, sunIntensity, horizonColor), 1.0f);
}
