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

// Push constant: HDR slot, tonemap operator, and pre-tonemap exposure.
struct TonemapPush
{
    uint  hdrSlot;
    uint  mode;     // 0 = Reinhard, 1 = ACES Filmic, 2 = Uncharted 2
    float exposure;
};
[[vk::push_constant]] TonemapPush pc;

// ── Vertex shader ──────────────────────────────────────────────────────────
// Produces a CCW triangle that covers the entire NDC clip-space:
//   VertexID 0 -> (-1, -1, 0, 1)   top-left  (NDC)
//   VertexID 1 -> ( 3, -1, 0, 1)   bottom-right overshoot
//   VertexID 2 -> (-1,  3, 0, 1)   top-right overshoot
// The rasteriser clips the overshoot, giving a full viewport triangle with
// no vertex buffer and no wasted fill from two-triangle quads.
[shader("vertex")]
float4 vertexMain(uint vertexID : SV_VertexID) : SV_Position
{
    float2 uv = float2((vertexID << 1u) & 2u, vertexID & 2u);
    return float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
}

// ── Tonemap operators ──────────────────────────────────────────────────────

// Reinhard (1/1+x): simple, cheap.  White point at infinity.
float3 Reinhard(float3 x)
{
    return x / (x + float3(1.0f));
}

// ACES Filmic - Narkowicz 2015 curve fit to the Academy RRT+ODT.
// Clipped to [0,1]; slight toe and shoulder, pleasant saturation.
float3 AcesFilmic(float3 x)
{
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0f, 1.0f);
}

// Uncharted 2 / John Hable - popular "filmic" curve with tunable constants.
float3 Uncharted2Partial(float3 x)
{
    const float A = 0.15f, B = 0.50f, C = 0.10f, D = 0.20f, E = 0.02f, F = 0.30f;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

float3 Uncharted2(float3 x)
{
    const float3 curr       = Uncharted2Partial(x * 2.0f);
    const float3 whiteScale = float3(1.0f) / Uncharted2Partial(float3(11.2f));
    return clamp(curr * whiteScale, 0.0f, 1.0f);
}

// ── Fragment shader ────────────────────────────────────────────────────────
[shader("fragment")]
float4 fragmentMain(float4 svPos : SV_Position) : SV_Target0
{
    const int2   coord = int2(svPos.xy);
    const float3 hdr   = g_textures[pc.hdrSlot].Load(int3(coord, 0)).rgb * pc.exposure;

    float3 mapped =
        (pc.mode == 1u) ? AcesFilmic(hdr) :
        (pc.mode == 2u) ? Uncharted2(hdr) :
                          Reinhard(hdr);

    // Approximate sRGB gamma (linear -> display).
    const float3 gamma = pow(max(mapped, float3(0.0f)), float3(1.0f / 2.2f));
    return float4(gamma, 1.0f);
}
