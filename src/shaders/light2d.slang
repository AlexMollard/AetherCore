// 2D screen-space light-map. Runs once, fullscreen, AFTER the sprite/tile layer has been drawn into
// the scene HDR colour and BEFORE tonemap, with MULTIPLY blend (DstColor, Zero) so the hardware does
//   hdrColor.rgb *= light.rgb
// The light map is: ambient (a dark floor) + the radial contribution of every point light and the
// cone contribution of every spot light, DIRECTIONALLY shaded off a surface normal derived from the
// occluder mask (so geometry looks sculpted and light stops at solids) and cast-shadowed by walls.
// Consumes the SAME lights the 3D renderer uses (engine PointLight/SpotLight components).
//
// World reconstruction is exact for a 2D ortho camera: world.xy depends only on screen ndc, not depth.
#include "include/DevicePointer.slangh"
#include "include/FrameConstants.slangh"

[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

// Packed light record (64 bytes). Mirrors aether::GpuLight2D in Light2DCompositor.cpp.
struct GpuLight2D
{
    float4 posRadiusKind;  // xy = world position, z = radius (world units), w = kind (0 point, 1 spot)
    float4 colorIntensity; // rgb = colour, a = intensity (peak added brightness at the centre)
    float4 spotDirCone;    // xy = aim direction (2D, normalized), z = cosInner, w = cosOuter
    float4 flags;          // x = castsShadow (0/1)
};

struct Light2DPush
{
    DevicePtr<FrameConstantsData> frameConstants;
    DevicePtr<GpuLight2D> lights;
    uint   count;          // number of GpuLight2D records
    uint   occluderSlot;   // bindless slot of the R8 occluder mask (0xFFFFFFFF = no mask)
    float2 viewportSize;
    float4 ambient;        // rgb = ambient floor, a = per-channel clamp ceiling
    float4 shadowParams;   // x = cast-shadows enabled, y = strength, z = softness, w = world bias
    float4 shaftParams;    // x = light-shaft strength (0 = off), y = medium density variation
};

[[vk::push_constant]] Light2DPush pc;

struct VSOutput
{
    float4 position : SV_Position;
};

[shader("vertex")]
VSOutput vertexMain(uint vertexId : SV_VertexID)
{
    // Fullscreen quad in clip space (two triangles).
    const float2 corners[6] = {
        float2(0, 0), float2(1, 0), float2(1, 1),
        float2(0, 0), float2(1, 1), float2(0, 1),
    };
    const float2 c = corners[vertexId % 6u];
    VSOutput o;
    o.position = float4(c * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

float SampleOccluder(float2 uv)
{
    return g_textures[pc.occluderSlot].SampleLevel(g_linearSampler, uv, 0.0).r;
}

GpuLight2D LoadLight2D(uint index) { return pc.lights.Ptr()[index]; }
#include "include/Light2DMath.slangh"

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    FrameConstantsData* frame = pc.frameConstants.Ptr();
    const float2 uv = input.position.xy / max(pc.viewportSize, float2(1.0, 1.0));
    return EvaluateLight2D(uv, frame->viewProj, frame->invViewProj, frame->elapsedTime, pc.count, pc.ambient, pc.shadowParams, pc.shaftParams, pc.occluderSlot != 0xFFFFFFFFu);
}
